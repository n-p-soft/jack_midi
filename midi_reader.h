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

#define MIDI_READER_VERSION	104

/* state of MIDI frame */
typedef enum midi_frame_state_t {
	MIDIF_IOERROR = -3, /* could not read data */
	MIDIF_NODATA = -2, /* no byte read */
	MIDIF_ERROR = -1, /* error, frame was reset */
	MIDIF_NEXT = 0, /* waiting for next byte */
	MIDIF_COMPLETE = 1, /* frame is complete */
	MIDIF_SKIPPED = 2, /* frame complete but skipped */
} midi_frame_state_t;

/* max count of bytes in a MIDI frame */
#define MIDI_FRAME_MAX	128

/* MIDI frame */
typedef struct midi_frame_t {
	unsigned char len; /* current length */
	unsigned char data[MIDI_FRAME_MAX]; /* data bytes */
} midi_frame_t;

/* max count of frames in midi_frames_t */
#define MIDI_READER_FRAMES_MAX	1024

/* an array of MIDI frames */
typedef struct midi_frames_t {
	int len; /* current length */
	int offset; /* current offset */
	midi_frame_t frames[MIDI_READER_FRAMES_MAX]; /* the frames */
} midi_frames_t;

/* flags for the MIDI reader */
typedef enum midi_reader_flags_t
{
	MIDIR_NONE = 0,
	MIDIR_DEBUG = 1, /* show frame content when it is read */
	MIDIR_EXPAND = 2, /* expand running status frames */
	MIDIR_DUMPHEX = 4, /* dump in hex format, not binary */
} midi_reader_flags_t;

/* User callback function called each time a MIDI frame is read and validated.
 * When it returns MIDIF_COMPLETE, the frame is also stored in the internal
 * queue and so will be returned by a call to "midi_reader_get_next".
 * When it returns MIDIF_SKIPPED or another value, the frame is not stored in
 * the queue nor dump'ed.
 */
typedef midi_frame_state_t (*midi_reader_callback_t) (midi_frame_t* mf,
							void *user_data);

/* max length of read buffer */
#define MIDI_READER_BUF_MAX	256

/* max count of input devices */
#define MIDI_READER_IN_MAX	64

/* input buffer */
typedef unsigned char midi_reader_buf_t[MIDI_READER_BUF_MAX];

/* statistics for midi reader. */
typedef struct midi_reader_stats_t
{
	unsigned long read; /* count of frames read */
	unsigned long errors; /* count of erroneous incoming frames */
	unsigned long skipped; /* count of frames read but skipped */
	unsigned long missed; /* frames not stored in queue */
} midi_reader_stats_t;

/* source of data */
typedef struct midi_reader_source_t {
	int fd; /* file descriptors to read from */
	unsigned char running; /* current running status command or 0 */
	midi_reader_buf_t buf; /* input buffer */
	int buf_len; /* current buf length */
	int buf_offset; /* current offset in buf */
	int push_back; /* byte pushed-back or -1 if none */
	midi_frame_t current; /* frame being parsed */
	int channel; /* if 1-16, channel to update */
	midi_reader_stats_t stats;
} midi_reader_source_t;

/* used to read bytes and store MIDI frames */
typedef struct midi_reader_t
{
	midi_reader_flags_t flags; /* reader flags */
	midi_reader_source_t sources[MIDI_READER_IN_MAX]; /* input sources */
	int nsources; /* count of input devices */
	int dumpfd; /* dump file descriptor */
	midi_frames_t frames; /* frames that were read */
	const unsigned char *to_skip; /* status bytes to skip */
	midi_reader_callback_t callback; /* callback function */
	void *user_data; /* user data for callback */
	midi_reader_stats_t total; /* cumulated stats */
} midi_reader_t;

/* list of possible MIDI frames length indexed by the status byte.
 * -1: error, -xx: variable length, > 0 fixed (minimal for running status)
 * length
 */
static const int midi_frame_len[128] = {
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

/* Get the version of the library as a 3-digits number (100, 101,..). */
int midi_reader_get_version ();

/* Initialize a MIDI reader. 'to_skip' may be NULL or a pointer to a ZERO-
 * terminated array of status bytes; frames starting with one of these bytes
 * will be skipped. User should call "midi_reader_add_source" after this.
 */
void
midi_reader_init (midi_reader_t* reader, midi_reader_flags_t flags,
			const unsigned char *to_skip);

/* Add a MIDI-in file descriptor to the reader. Return false on failure.
 * If 'channel' is a value between 1 and 16, then the channel 'n' for all
 * channel-type messages (0x8n-0xEn) is changed to this value.
 */
bool
midi_reader_add_source (midi_reader_t *reader, int fd, int channel);
			
/* Same as "midi_reader_add_source" but using a file path. */
bool
midi_reader_add_source_path (midi_reader_t *reader,
				const char *path, int channel);

/* Remove a MIDI-in file descriptor from the reader. Return false on failure. */
bool
midi_reader_remove_source (midi_reader_t *reader, int fd);

/* Set the file descriptor where to dump frames. Dump file will be closed if
 * function "midi_reader_close" is called.
 * Returns false on error.
 */
bool
midi_reader_set_dump_fd (midi_reader_t *reader, int fd);

/* Same as "midi_reader_set_dump_fd" but using a file path (file will be created
 * and also truncated if "trunc" is true).
 */
bool
midi_reader_set_dump_file (midi_reader_t *reader, const char *path, bool trunc);

/* Set a user callback function with optional argument. Note that when using a
 * callback, you should call regularly "midi_reader_get_next" or
 * "midi_reader_clear_queue" so that the internal queue get not full.
 * That is, when a callback is set and a MIDI frame is validated, the callback
 * is invoked and the frame will be stored in the internal queue and dump'ed
 * if MIDIF_COMPLETE is returned. Else the frame is not stored nor dump'ed.
 * The callback occurs just before dumping the frame, so the data may be
 * properly changed.
 */
void
midi_reader_set_callback (midi_reader_t *reader,
			midi_reader_callback_t cb, void *user_data);

/* Close a MIDI reader. Note that "midi_reader_get_next" may be called after
 * this until the frames already read and stored in the internal buffer are
 * exhausted, but no new frame will be read.
 */
void
midi_reader_close (midi_reader_t* reader);

/* Returns -1 if no midi device is readable, 0 if there is no byte to
 * read, else the count of MIDI-in devices having data to read from.
 */
int
midi_reader_poll (midi_reader_t* reader);

/* Inject a MIDI frame that will be processed immediately.
 * Beware that the frame will appear when calling "midi_reader_get_next" and
 * thru the user callback, if any.
 * Return the count of bytes processed. May be less than the length of the
 * frame in case or error (erroneous frame, internal queue is full, ..).
 * Note: only one valid frame may be injected at once.
 */
int
midi_reader_inject (midi_reader_t *reader, midi_frame_t *mf);

/* Inject "n" bytes that will be processed immediately.
 * See "midi_reader_inject" for more details.
 */
int
midi_reader_inject_bytes (midi_reader_t *reader, int n, ...);

/* Reset a MIDI frame. */
void
midi_frame_reset (midi_frame_t* mf);

/* Dump the frame content. */
void
midi_frame_dump (midi_frame_t *mf, int fd);

/* Return true if there is a MIDI frame that was read. Should be called
 * regularly to read new frames and store them in the internal buffer. */
bool
midi_reader_update (midi_reader_t *reader);

/* Return next valid MIDI frame read by the reader, or NULL if none. */
midi_frame_t*
midi_reader_get_next (midi_reader_t *reader);

/* Remove all recorded frames. */
void
midi_reader_clear_queue (midi_reader_t *reader);

/* Get the statistics for the nth input source (0..; if -1: cumulated).
 * Returns false on failure.
 */
bool
midi_reader_get_stats (midi_reader_t *reader,
			int n, midi_reader_stats_t *stats);

/* Reset statistics for nth source (n=0..) or global ones (n=-1). */
void
midi_reader_reset_stats (midi_reader_t *reader, int n);

#ifdef __cplusplus
} /* extern C */
#endif

#endif /* MIDI_READER_H */
