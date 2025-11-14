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
#include "midi_reader.h"

int
midi_reader_poll (midi_reader_t *reader)
{
	if (reader == NULL || reader->fd < 0)
		return (-1);
	else {
		struct pollfd pfd[1];

		pfd[0].fd = reader->fd;
		pfd[0].events = POLLIN | POLLPRI;
		return (poll(pfd, 1, 0));
	}
}

void
midi_reader_init (midi_reader_t *reader, midi_reader_flags_t flags,
			const unsigned char *to_skip)
{
	if (reader) {
		memset (reader, 0, sizeof (midi_reader_t));
		reader->flags = flags;
		reader->fd = -1;
		reader->dumpfd = -1;
		reader->to_skip = to_skip;
		reader->push_back = -1;
	}
}

void
midi_reader_close (midi_reader_t *reader)
{
	if (reader) {
		close (reader->fd);
		reader->fd = -1;
		reader->push_back = -1;
	}
}

void
midi_reader_set_fd (midi_reader_t *reader, int fd)
{
	if (reader)
		reader->fd = fd;
}

void
midi_reader_set_dump_fd (midi_reader_t *reader, int fd)
{
	if (reader)
		reader->dumpfd = fd;
}

static int
midi_reader_get_byte (midi_reader_t *reader)
{
	int r;

	if (reader == NULL)
		return (-1);
	if (reader->push_back > -1) {
		r = reader->push_back;
		reader->push_back = -1;
		return (r);
	}
	if (reader->buf_offset >= reader->buf_len) {
		reader->buf_offset = 0;
		reader->buf_len = 0;
	}
	if (reader->buf_len < MIDI_READER_BUF_MAX) {
		r = read (reader->fd, (char*) &reader->buf[reader->buf_len],
				MIDI_READER_BUF_MAX - reader->buf_len);

		if (r > 0)
			reader->buf_len += r;
	}
	if (reader->buf_len > 0 && reader->buf_offset < reader->buf_len)
		return (reader->buf[reader->buf_offset++]);
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
midi_frame_process (midi_reader_t *reader, midi_frame_t *mf)
{
	if (reader->to_skip) {
		const unsigned char *p;

		for (p = reader->to_skip; *p; p++) {
			if (mf->data[0] == *p)
				break;
		}
		if (*p) {
			if (reader->flags & MIDIR_DEBUG) {
				dprintf (2, "skipped frame: ");
				midi_frame_dump (2, mf);
				dprintf (2, "\n");
			}
			midi_frame_reset (mf);
			return (MIDIF_NEXT);
		}
	}
	if (reader->flags & MIDIR_EXPAND)
		midi_frame_expand_running (mf);
	if (reader->dumpfd > 0) {
		if (reader->flags & MIDIR_DUMPHEX)
			midi_frame_dump (reader->dumpfd, mf);
		else
			dprintf (reader->dumpfd, (char*) mf->data, mf->len);
	}
	return (MIDIF_COMPLETE);
}

midi_frame_state_t
midi_frame_push (midi_reader_t *reader, midi_frame_t *mf)
{
	int len;
	int b;

	if (reader == NULL || mf == NULL)
		return (MIDIF_NODATA);
	if (mf->len == MIDI_FRAME_MAX) {
		/* error, too long frame */
		midi_frame_reset (mf);
		reader->running = 0;
		return (MIDIF_ERROR);
	}
	b = midi_reader_get_byte (reader);
	if (b < 0)
		return (MIDIF_NODATA);
	else if (b > 0xFF)
		return (MIDIF_IOERROR);
	if (reader->running != 0 && (b & 0x80) != 0) {
		reader->push_back = b;
		reader->running = 0;
		if (mf->len == 0 || ((mf->len - 1) % 2)) {
			midi_frame_reset (mf);
			return (MIDIF_ERROR);
		}
		else
			return (midi_frame_process (reader, mf));
	}
	if (mf->len == 0) {
		if (b >= 0x80 && b <= 0xef)
			reader->running = b;
		else
			reader->running = 0;
	}

	mf->data[mf->len++] = (unsigned char) b;
	len = midi_frame_len[mf->data[0]];
	if (len == -1) {
		/* error */
		reader->running = 0;
		midi_frame_reset (mf);
		return (MIDIF_ERROR);
	}
	else if (len == -0xf0 && mf->len > 1) {
		/* system exclusive */
		if (b == 0xf7)
			return (midi_frame_process (reader, mf));
	}
	else if (reader->running == 0 && len == mf->len)
		return (midi_frame_process (reader, mf));

	return (MIDIF_NEXT);
}

void
midi_frame_dump (int fd, midi_frame_t *mf)
{
	if (fd > -1 && mf) {
		for (int j = 0; j < mf->len; j++)
			dprintf (fd, "%.2x ", mf->data[j]);
	}
}

static midi_frame_state_t
midi_reader_read_one (midi_reader_t *reader)
{
	midi_frame_state_t r;
	midi_frame_t *current;

	current = &reader->frames.frames[reader->frames.len];
	r = midi_frame_push (reader, current);
	if (r == MIDIF_COMPLETE) {
		if (reader->flags & MIDIR_DEBUG) {
			dprintf (2, "read frame#%i: ", reader->frames.len);
			midi_frame_dump (2, current);
			dprintf (2, "\n");
		}
		reader->frames.len++;
	}
	return (r);
}

bool
midi_reader_update (midi_reader_t *reader)
{
	if (reader == NULL)
		return (false);
	else if (reader->frames.len == 0)
		return (midi_reader_read_one (reader) == MIDIF_COMPLETE);
	else if (reader->frames.len < MIDI_FRAMES_MAX) {
		if (reader->frames.offset < reader->frames.len)
			return (true);
		else {
			return (midi_reader_read_one (reader) ==
							MIDIF_COMPLETE);
		}
	}
	else {
		if (reader->frames.offset < reader->frames.len)
			return (true);
		else {
			memset (&reader->frames, 0, sizeof (midi_frames_t));
			return (midi_reader_read_one (reader) ==
							MIDIF_COMPLETE);
		}
	}
}

midi_frame_t*
midi_reader_get_next (midi_reader_t *reader)
{
	if ( ! midi_reader_update (reader))
		return (NULL);
	else if (reader->frames.len > 0 &&
		reader->frames.offset < reader->frames.len)
		return (&reader->frames.frames[reader->frames.offset++]);
	else
		return (NULL);
}

void
midi_reader_clear_queue (midi_reader_t *reader)
{
	if (reader->frames.len > 0) {
		memcpy (&reader->frames.frames[0],
			&reader->frames.frames[reader->frames.len],
			sizeof (midi_frame_t));
		reader->frames.len = 0;
		reader->frames.offset = 0;
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

