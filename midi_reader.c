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

#include <midi_reader.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>

bool
midi_reader_poll (midi_reader_t *reader)
{
	if (reader == NULL || reader->fd < 0)
		return (false);
	else {
		struct pollfd pfd[1];

		pfd[0].fd = reader->fd;
		pfd[0].events = POLLIN | POLLPRI;
		return (poll(pfd, 1, 0) > 0);
	}
}

void
midi_reader_init (midi_reader_t *reader, int fd,
			const unsigned char *to_skip)
{
	if (reader) {
		memset (reader, 0, sizeof (midi_reader_t));
		reader->fd = fd;
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

midi_frame_state_t
midi_frame_push (midi_reader_t *reader, midi_frame_t *mf)
{
	int len;
	int b;

	if (reader == NULL || mf == NULL)
		return (MIDIF_NOBYTE);
	if (mf->len == MIDI_FRAME_MAX) {
		/* error, too long frame */
		midi_frame_reset (mf);
		reader->running = 0;
		return (MIDIF_ERROR);
	}
	b = midi_reader_get_byte (reader);
	if (b < 0)
		return (MIDIF_NOBYTE);
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
			return (MIDIF_COMPLETE);
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
			return (MIDIF_COMPLETE);
	}
	else if (reader->running == 0 && len == mf->len)
		return (MIDIF_COMPLETE);

	return (MIDIF_NEXT);
}

static bool
midi_frame_skip (midi_reader_t *reader, midi_frame_t *mf)
{
	const unsigned char *p;

	if (reader && reader->to_skip && mf) {
		for (p = reader->to_skip; *p; p++) {
			if (mf->data[0] == *p)
				return (true);
		}
	}
	return (false);
}

void
midi_frame_dump (int fd, midi_frame_t *mf)
{
	if (fd > -1 && mf) {
		for (int j = 0; j < mf->len; j++)
			dprintf (fd, "%x ", mf->data[j]);
	}
}

midi_frame_t*
midi_reader_get_next (midi_reader_t *reader)
{
	if (reader == NULL)
		return (NULL);
----
	else if (reader->frames.offset >= reader->frames.len)
		return (&reader->frames.frames[reader->frames.offset++];
	else if (midi_reader
}

midi_frame_state_t
midi_reader_read_one (midi_reader_t *reader)
{
	midi_frame_state_t r;
	midi_frame_t *current;

	if (reader == NULL || reader->frames.len >= MIDI_FRAMES_LEN)
		return (MIDIF_ERROR);

	current = &reader->frames.frames[reader->frames.len];
	r = midi_frame_push (reader, current);
	if (r == MIDIF_COMPLETE) {
		if ( ! midi_frame_skip (reader, current)) {
			if (reader->flags & DEBUG) {
				dprintf (2, "read frame#%i: ", midi_frames_len);
				midi_frame_dump (2, current);
				dprintf (2, "\n");
			}
			reader->frames.len++;
		}
		else {
			if (reader->flags & DEBUG) {
				dprintf (2, "skipped frame#%i: ",
						reader->frames.len);
				midi_frame_dump (2, current);
				dprintf (2, "\n");
			}
			midi_frame_reset (current);
		}
	}
	return (r);
}

void
midi_frame_expand_running (midi_frame_t *mf)
{
	if (mf == NULL || mf->data[0] < 0x80 ||
		(mf->data[0] > 0xef || mf->len == 3) ||
		((mf->len - 1) % 2))
		return;
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
	}
}

