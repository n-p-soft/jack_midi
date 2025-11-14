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

#ifndef MIDI_READER_H
#define MIDI_READER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* state of MIDI frame */
typedef enum midi_frame_state_t {
	MIDIF_IOERROR = -3, /* could not read data */
	MIDIF_NODATA = -2, /* no byte read */
	MIDIF_ERROR = -1, /* error, frame was reset */
	MIDIF_NEXT = 0, /* waiting for next byte */
	MIDIF_COMPLETE = 1, /* frame is complete */
	MIDIF_SKIPPED = 2, /* frame complete but skipped and reset */
} midi_frame_state_t;

/* max count of bytes in a MIDI frame */
#define MIDI_FRAME_MAX	128

/* MIDI frame */
typedef struct midi_frame_t {
	unsigned char len; /* current length */
	unsigned char data[MIDI_FRAME_MAX]; /* data bytes */
} midi_frame_t;

/* max count of frames in midi_frames_t */
#define MIDI_FRAMES_MAX	256

/* an array of MIDI frames */
typedef struct midi_frames_t {
	int len; /* current length */
	int offset; /* current offset */
	midi_frame_t frames[MIDI_FRAMES_MAX]; /* the frames */
} midi_frames_t;

/* flags for the MIDI reader */
typedef enum midi_reader_flags_t
{
	MIDIR_NONE = 0,
	MIDIR_DEBUG = 1, /* show frame content when it is read */
	MIDIR_EXPAND = 2, /* expand running status frames */
} midi_reader_flags_t;

/* max length of read buffer */
#define MIDI_READER_BUF_MAX	 256

/* used to read bytes and store MIDI frames */
typedef struct midi_reader_t
{
	midi_reader_flags_t flags; /* reader flags */
	int fd; /* file descriptor to read from */
	unsigned char running; /* current running status command or 0 */
	midi_frames_t frames; /* frames that were read */
	const unsigned char *to_skip; /* if not NULL, 0-terminated array of
			* status bytes for which frames will be skipped. */
	unsigned char buf[MIDI_READER_BUF_MAX];
	int buf_len; /* current buf length */
	int buf_offset; /* current offset in buf */
	int push_back; /* byte that was pushed back or -1 if none */
} midi_reader_t;

/* list of possible MIDI frames length indexed by the status byte.
 * -1: error, -xx: variable length, > 0 fixed (minimal for running status)
 * length
 */
static const int midi_frame_len[256] = {
	/* 0 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 10 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 20 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 30 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 40 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 50 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 60 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 70 */ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	/* 80 note off */ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/* 90 note on */  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/* A0 aftertouch */  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/* B0 control change */ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/* C0 program change */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* D0 pressure */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	/* E0 pitch bend */  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	/* F0 system common */ -0xf0, 2, 3, 2, 1, 1, 1, 1,
	/* F8 system real-time */ 1, 1, 1, 1, 1, 1, 1, 1
};

/* Initialize a MIDI reader. 'to_skip' may be NULL or a pointer to a ZERO-
 * terminated array of status bytes which coreesponding frames will be skipped.
 */
void
midi_reader_init (midi_reader_t* reader, midi_reader_flags_t flags,
			int fd, const unsigned char *to_skip);

/* Change the device descriptor into the reader. */
void
midi_reader_set_fd (midi_reader_t *reader, int fd);
			
/* Close a MIDI reader. Note that midi_reader_get_next may be called after
 * this until the frames already read are exhausted, but no new frame will
 * be read.
 */
void
midi_reader_close (midi_reader_t* reader);

/* Returns -1 if the midi device is no more readable, 0 if there is no byte to
 * read, else 1.
 */
int
midi_reader_poll (midi_reader_t* reader);

/* Reset a MIDI frame. */
void
midi_frame_reset (midi_frame_t* mf);

/* Read a byte using the reader and push it in the MIDI frame. Return value:
 * MIDIF_IOERROR: could not read byte
 * MIDIF_NOBYTE: no byte read
 * MIDIF_ERROR: byte received could not be pushed, frame was reset
 * MIDIF_COMPLETE: byte pushed, frame is complete
 * MIDIF_NEXT: byte pushed, frame not yet completed
 * MIDIF_SKIPPED: byte pushed, frame complete but skipped (and reset)
 */
midi_frame_state_t
midi_frame_push (midi_reader_t* reader, midi_frame_t* mf);

/* Dump the frame content. */
void
midi_frame_dump (int fd, midi_frame_t *mf);

/* Return next valid MIDI frame read by the reader, or NULL if none. */
midi_frame_t*
midi_reader_get_next (midi_reader_t *reader);

/* Expand given MIDI frame if it is a running status one. May return false if
 * there was not enough room available in 'mf' (but this should not occur).
 */
bool
midi_frame_expand_running (midi_frame_t *mf);

#ifdef __cplusplus
} /* extern C */
#endif

#endif /* MIDI_READER_H */
