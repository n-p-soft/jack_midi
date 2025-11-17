/*-
 * Copyright (c) 2025 Nicolas Provost <dev@nicolas-provost.fr>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "midi_reader.h"

int
midi_reader_get_version ()
{
	return (MIDI_READER_VERSION);
}

static int
midi_reader_poll_src (midi_reader_t *reader, int *src)
{
	*src = -1;
	if (reader == NULL || reader->nsources == 0)
		return (-1);
	else {
		struct pollfd pfd[MIDI_READER_IN_MAX];
		int r, i;

		for (i = 0; i < reader->nsources; i++) {
			pfd[i].fd = reader->sources[i].fd;
			pfd[i].events = POLLIN | POLLPRI;
			pfd[i].revents = 0;
		}
		r = poll (pfd, reader->nsources, 0);
		if (r <= 0)
			return (r);
		for (*src = 0; *src < reader->nsources; (*src)++) {
			if (pfd[*src].revents & (POLLIN | POLLPRI))
				break;
		}
		return (r);
	}
}

int
midi_reader_poll (midi_reader_t *reader)
{
	int src;

	return (midi_reader_poll_src (reader, &src));
}

static void
midi_reader_reset_source (midi_reader_source_t *src, bool to_close)
{
	if (src) {
		memset (src, 0, sizeof (midi_reader_source_t));
		if (to_close) {
			close (src->fd);
			src->fd = -1;
		}
		src->push_back = -1;
		src->channel = -1;
	}
}

static void
midi_reader_reset_source_n (midi_reader_t *reader, int src, bool to_close)
{
	if (src >= 0 && src < MIDI_READER_IN_MAX)
		midi_reader_reset_source (&reader->sources[src], to_close);
}

void
midi_reader_init (midi_reader_t *reader, midi_reader_flags_t flags,
			const unsigned char *to_skip)
{
	if (reader) {
		memset (reader, 0, sizeof (midi_reader_t));
		reader->flags = flags;
		for (int i = 0; i < MIDI_READER_IN_MAX; i++)
			midi_reader_reset_source_n (reader, i, false);
		reader->dumpfd = -1;
		reader->to_skip = to_skip;
	}
}

bool
midi_reader_add_source_path (midi_reader_t *reader,
				const char *path, int channel)
{
	if (path) {
		int fd = open (path, O_RDONLY | O_NONBLOCK);

		if (fd > -1) {
			if ( ! midi_reader_add_source (reader, fd, channel))
				close (fd);
			else
				return (true);
		}
	}
	return (false);
}

bool
midi_reader_add_source (midi_reader_t *reader, int fd, int channel)
{
	if (reader && fd > -1 && reader->nsources < MIDI_READER_IN_MAX) {
		for (int i = 0; i < reader->nsources; i++) {
			if (reader->sources[i].fd == fd)
				return (true);
		}
		reader->sources[reader->nsources].fd = fd;
		if (channel >= 1 && channel <= 16)
			reader->sources[reader->nsources].channel = channel;
		else
			reader->sources[reader->nsources].channel = -1;
		reader->nsources++;
		return (true);
	}
	return (false);
}

bool
midi_reader_remove_source (midi_reader_t *reader, int fd)
{
	int i, j;

	if (reader == NULL || fd < 0 || reader->nsources == 0)
		return (false);

	for (i = 0; i < reader->nsources; i++) {
		if (reader->sources[i].fd == fd)
			break;
	}
	if (i >= reader->nsources)
		return (false);
	else {
		midi_reader_reset_source_n (reader, i, true);
		for (j = i + 1; j < MIDI_READER_IN_MAX; j++)
			reader->sources[j - 1] = reader->sources[j];
		midi_reader_reset_source_n (reader, j, false);
		reader->nsources--;
		return (true);
	}
}

bool
midi_reader_set_dump_fd (midi_reader_t *reader, int fd)
{
	if (reader && fd > -1) {
		reader->dumpfd = fd;
		return (true);
	}
	return (false);
}

bool
midi_reader_set_dump_file (midi_reader_t *reader,
				const char *path, bool trunc)
{
	if (reader && path) {
		int mode = O_CREAT | O_WRONLY | O_CLOEXEC;
		int fd;

		if (trunc)
			mode |= O_TRUNC;	
		fd = open (path, mode, 0600);
		if (fd > -1) {
			if ( ! midi_reader_set_dump_fd (reader, fd))
				close (fd);
			else
				return (true);
		}
	}
	return (false);
}

void
midi_reader_set_callback (midi_reader_t *reader,
			midi_reader_callback_t cb, void *user_data)
{
	if (reader) {
		reader->callback = cb;
		reader->user_data = user_data;
	}
}

static void
midi_reader_read (midi_reader_t *reader)
{
	int r;
	midi_reader_source_t *s;

	for (int i = 0; i < reader->nsources; i++) {
		s = &reader->sources[i];
		if (s->push_back > -1)
			continue;
		if (s->buf_offset >= s->buf_len) {
			s->buf_len = 0;
			s->buf_offset = 0;
		}
		if (s->buf_len >= MIDI_READER_BUF_MAX)
			continue;
		r = read (s->fd, s->buf + s->buf_len,
				MIDI_READER_BUF_MAX - s->buf_len);
		if (r > 0)
			s->buf_len += r;
	}
}

static int
midi_reader_get_byte (midi_reader_t *reader, int src)
{
	int r;
	midi_reader_source_t *s = &reader->sources[src];

	if (s->push_back > -1) {
		/* requested source has a push-backed byte */
		r = s->push_back;
		s->push_back = -1;
		return (r);
	}
	else if (s->buf_len > 0 && s->buf_offset < s->buf_len) {
		/* requested source has a buffered byte */
		return (s->buf[s->buf_offset++]);
	}
	else
		return (-1);
}

void
midi_frame_reset (midi_frame_t *mf)
{
	if (mf) {
		mf->len = 0;
		mf->data[0] = 0;
	}
}

static midi_frame_state_t
midi_frame_process (midi_reader_t *reader, midi_frame_t *mf,
			midi_reader_source_t *src)
{
	bool skipped = false;
	const unsigned char *p;
	midi_frame_state_t st;

	if (mf->len == 0)
		return (MIDIF_NODATA);
	src->stats.read++;
	reader->total.read++;
	if (reader->to_skip) {
		for (p = reader->to_skip; *p; p++) {
			if (mf->data[0] == *p) {
				skipped = true;
				break;
			}
		}
	}
	if (reader->flags & MIDIR_DEBUG) {
		dprintf (2, "incoming frame%s",
				skipped ? " (skipped): " : ": ");
		midi_frame_dump (mf, 2);
		dprintf (2, "\n");
	}
	if (skipped) {
		src->stats.skipped++;
		reader->total.skipped++;
		return (MIDIF_SKIPPED);
	}

	/* channel translation */
	if (src->channel > 0) {
		if (mf->data[0] >= 0x80 && mf->data[0] <= 0xef) {
			mf->data[0] = mf->data[0] & 0xF0;
			mf->data[0] |= src->channel - 1;
		}
	}

	/* running-status expansion */
	if (reader->flags & MIDIR_EXPAND)
		midi_frame_expand_running (mf);

	/* user callback */
	if (reader->callback) {
		st = reader->callback (mf, reader->user_data);
		if (mf->len == 0)
			return (MIDIF_NODATA);
		else if (st == MIDIF_SKIPPED) {
			src->stats.skipped++;
			reader->total.skipped++;
			return (MIDIF_SKIPPED);
		}
		else if (st != MIDIF_COMPLETE) {
			src->stats.errors++;
			reader->total.errors++;
			return (st);
		}
	}

	/* dump */
	if (reader->dumpfd > -1) {
		if (reader->flags & MIDIR_DUMPHEX)
			midi_frame_dump (mf, reader->dumpfd);
		else
			write (reader->dumpfd, (char*) mf->data, mf->len);
	}

	/* store */
	if (reader->frames.len == reader->frames.offset) {
		reader->frames.len = 0;
		reader->frames.offset = 0;
	}
	if (reader->frames.len < MIDI_READER_FRAMES_MAX) {
		memcpy (&reader->frames.frames[reader->frames.len++],
			mf, sizeof (midi_frame_t));
	}
	else
		reader->total.missed++;

	return (MIDIF_COMPLETE);
}

void
midi_reader_close (midi_reader_t *reader)
{
	int i;

	if (reader == NULL || reader->nsources == 0)
		return;

	for (i = 0; i < reader->nsources; i++)
		midi_reader_reset_source_n (reader, i, true);

	if (reader->dumpfd > -1) {
		close (reader->dumpfd);
		reader->dumpfd = -1;
	}
}

static midi_frame_state_t
midi_reader_push_byte (midi_reader_t *reader, midi_reader_source_t *src,
			int data)
{
	int len;
	midi_frame_t *mf = &src->current;
	unsigned char b;

	if (data < 0)
		return (MIDIF_NODATA);
	else if (data > 0xFF) {
		src->stats.errors++;
		src->running = 0;
		reader->total.errors++;
		return (MIDIF_IOERROR);
	}
	b = (unsigned char) data;
	if (mf->len == MIDI_FRAME_MAX) {
		/* error, too long frame */
		src->running = 0;
		src->stats.errors++;
		reader->total.errors++;
		return (MIDIF_ERROR);
	}
	if (src->running != 0 && (b & 0x80) != 0) {
		src->push_back = b;
		src->running = 0;
		if (mf->len == 0 || ((mf->len - 1) % 2)) {
			src->stats.errors++;
			reader->total.errors++;
			return (MIDIF_ERROR);
		}
		else
			return (midi_frame_process (reader, mf, src));
	}
	if (mf->len == 0) {
		if (b >= 0x80 && b <= 0xef)
			src->running = b;
		else
			src->running = 0;
	}

	mf->data[mf->len++] = (unsigned char) b;
	if ( ! (mf->data[0] & 0x80)) {
		/* bad byte */
		src->stats.errors++;
		src->running = 0;
		reader->total.errors++;
		return (MIDIF_ERROR);
	}

	len = midi_frame_len[mf->data[0] - 0x80];
	if (len == -0xf0 && mf->len > 1) {
		/* system exclusive */
		if (b == 0xf7)
			return (midi_frame_process (reader, mf, src));
	}
	else if (src->running == 0 && len == mf->len)
		return (midi_frame_process (reader, mf, src));

	return (MIDIF_NEXT);
}

int
midi_reader_inject (midi_reader_t *reader, midi_frame_t *mf)
{
	midi_reader_source_t src;
	midi_frame_state_t r;
	int i;

	if (reader == NULL || mf == NULL || mf->len == 0)
		return (0);
	midi_reader_reset_source (&src, false);
	src.fd = -1;
	for (i = 0; i < mf->len; i++) {
		r = midi_reader_push_byte (reader, &src, mf->data[i]);
		switch (r) {
		case MIDIF_COMPLETE:
		case MIDIF_NEXT:
			continue;
		default:
			return (i);
		}
	}
	/* push a fake active-sensing to conclude any pending running-status
	 * frame */
	if (src.running != 0)
		midi_reader_push_byte (reader, &src, 0xfe);
	return (i);
}

int
midi_reader_inject_bytes (midi_reader_t *reader, int n, ...)
{
	midi_frame_t f;
	va_list va;

	if (reader == NULL || n <= 0 || n > MIDI_FRAME_MAX)
		return (0);
	midi_frame_reset (&f);
	va_start (va, n);
	for (int i = 0; i < n; i++)
		f.data[i] = va_arg (va, int);
	va_end (va);
	f.len = n;
	return (midi_reader_inject (reader, &f));
}

void
midi_frame_dump (midi_frame_t *mf, int fd)
{
	if (fd > -1 && mf) {
		for (int j = 0; j < mf->len; j++)
			dprintf (fd, "%.2x ", mf->data[j]);
	}
}

bool
midi_reader_update (midi_reader_t *reader)
{
	static int start = -1;
	midi_frame_state_t r;
	midi_reader_source_t *s;
	int src, i, b;

	if (reader == NULL)
		return (false);

	/* read all sources */
	midi_reader_read (reader);

	/* fill the queue */
	if (++start >= reader->nsources)
		start = 0;
	for (src = start, i = 0; i < reader->nsources; i++, src++) {
		if (src >= reader->nsources)
			src = 0;

		s = &reader->sources[src];
		b = midi_reader_get_byte (reader, src);
		r = midi_reader_push_byte (reader, &reader->sources[src], b);
		switch (r) {
		case MIDIF_COMPLETE:
		case MIDIF_ERROR:
		case MIDIF_IOERROR:
		case MIDIF_SKIPPED:
			midi_frame_reset (&s->current);
			break;
		case MIDIF_NODATA:
		case MIDIF_NEXT:
			break;
		}
	}
	
	return (reader->frames.len > 0 &&
		reader->frames.offset < reader->frames.len);
}

midi_frame_t*
midi_reader_get_next (midi_reader_t *reader)
{
	if ( ! midi_reader_update (reader))
		return (NULL);
	else
		return (&reader->frames.frames[reader->frames.offset++]);
}

void
midi_reader_clear_queue (midi_reader_t *reader)
{
	if (reader->frames.len > 0) {
		memset (&reader->frames, 0,
			sizeof (midi_frames_t));
	}
}

bool
midi_frame_expand_running (midi_frame_t *mf)
{
	if (mf == NULL || mf->data[0] < 0x80 ||
		(mf->data[0] > 0xef || mf->len == 3))
		return (true);
	else if ((mf->len - 1) % 2)
		return (false);
	else if (mf->len + (mf->len - 1) / 2 > MIDI_FRAME_MAX)
		return (false);
	else {
		int i;
		midi_frame_t f;

		memcpy (&f, mf, sizeof (midi_frame_t));
		midi_frame_reset (mf);
		for (i = 1; i < f.len; i += 2) {
			mf->data[mf->len++] = f.data[0];
			mf->data[mf->len++] = f.data[i];
			mf->data[mf->len++] = f.data[i+1];
		}
		return (true);
	}
}

bool
midi_reader_get_stats (midi_reader_t *reader, int n, midi_reader_stats_t *stats)
{
	if (reader == NULL || n < -1 || n >= reader->nsources || stats == NULL)
		return (false);
	if (n == -1)
		*stats = reader->total;
	else
		*stats = reader->sources[n].stats;
	return (true);
}

void
midi_reader_reset_stats (midi_reader_t *reader, int n)
{
	if (reader == NULL || n < -1 || n >= reader->nsources)
		return;
	else if (n == -1)
		memset (&reader->total, 0, sizeof (midi_reader_stats_t));
	else {
		memset (&reader->sources[n].stats, 0,
			sizeof (midi_reader_stats_t));
	}

}

