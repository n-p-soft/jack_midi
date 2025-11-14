/*-
 * Copyright (c) 2025 Nicolas Provost <dev@nicolas-provost.fr>
 * Copyright (c) 2011-2021 Hans Petter Selasky <hselasky@FreeBSD.org>
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <err.h>
#include <errno.h>
#include <sysexits.h>
#include <sys/errno.h>
#ifdef HAVE_SYSCTL
#include <sys/sysctl.h>
#endif
#include <pwd.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#define	JACK_PORT_NAME		"jack_midi"

#define	JACK_OUT_MAX	17		/* units */

static jack_port_t *output_port[JACK_OUT_MAX];
static jack_port_t *input_port;
static jack_client_t *jack_client;
static int read_offset;
static int read_len;
static int read_fd = -1;
static int write_fd = -1;
static int kill_on_close;
static int debug_mode;
static int expand;
static int background;
static char *read_name = NULL;
static char *write_name = NULL;
static char *port_name = NULL;
static pthread_mutex_t jack_midi_mtx;
static uid_t uid;
static int uid_found;

#define MIDI_FRAME_MAX	32
typedef struct midi_frame_t {
	uint8_t len;
	uint8_t	data[MIDI_FRAME_MAX*2];
} midi_frame_t;

#define MIDI_FRAMES_LEN	256
static midi_frame_t midi_frames[MIDI_FRAMES_LEN];
static int midi_frames_len;
static int midi_frames_offset;
static uint8_t midi_running;
static jack_nframes_t jack_counter;

#ifdef HAVE_DEBUG
#define	DPRINTF(fmt, ...) printf("%s:%d: " fmt, __FUNCTION__, __LINE__,## __VA_ARGS__)
#else
#define	DPRINTF(fmt, ...) do { } while (0)
#endif

static void
jack_midi_lock()
{
	pthread_mutex_lock(&jack_midi_mtx);
}

static void
jack_midi_unlock()
{
	pthread_mutex_unlock(&jack_midi_mtx);
}

static uid_t
jack_midi_id(const char *name, const char *type)
{
	uid_t val;
	char *ep;

	val = strtoul(name, &ep, 10);
	if (*ep != '\0')
		errx(EX_USAGE, "%s: illegal %s name", name, type);
	return (val);
}

static void
jack_midi_uid(const char *s)
{
	struct passwd *pw;

	uid = ((pw = getpwnam(s)) != NULL) ? pw->pw_uid : jack_midi_id(s, "user");
	uid_found = 1;
}

static void
jack_midi_write(jack_nframes_t nframes)
{
	int error;
	int events;
	int i;
	void *buf;
	jack_midi_event_t event;

	if (input_port == NULL)
		return;

	buf = jack_port_get_buffer(input_port, nframes);
	if (buf == NULL) {
		DPRINTF("jack_port_get_buffer() failed, cannot receive anything.\n");
		return;
	}
#ifdef JACK_MIDI_NEEDS_NFRAMES
	events = jack_midi_get_event_count(buf, nframes);
#else
	events = jack_midi_get_event_count(buf);
#endif

	for (i = 0; i < events; i++) {
#ifdef JACK_MIDI_NEEDS_NFRAMES
		error = jack_midi_event_get(&event, buf, i, nframes);
#else
		error = jack_midi_event_get(&event, buf, i);
#endif
		if (error) {
			DPRINTF("jack_midi_event_get() failed, lost MIDI event.\n");
			continue;
		}
		jack_midi_lock();
		if (write_fd > -1) {
			if (write(write_fd, event.buffer, event.size) != event.size) {
				DPRINTF("write() failed.\n");
			}
		}
		jack_midi_unlock();
	}
}

static void
jack_midi_read_push_back (int c)
{
	midi_read_back = c;
}

static inline int
jack_midi_read_byte(int fd)
{
	int r;

	if (midi_read_back > -1) {
		r = midi_read_back;
		midi_read_back = -1;
		return (r);
	}
	if (read_buf_offset >= read_buf_len) {
		read_buf_offset = 0;
		read_buf_len = 0;
	}
	if (read_buf_len < READ_BUF_MAX && jack_midi_poll (fd) > 0) {
		r = read (fd, (char*) &read_buf[read_buf_len],
				READ_BUF_MAX - read_buf_len);

		if (r > 0)
			read_buf_len += r;
	}
	if (read_buf_len > 0 && read_buf_offset < read_buf_len)
		return (read_buf[read_buf_offset++]);
	else
		return (-1);
}

/* -1: error, -xx: variable */
static const int midi_cmd_len[256] = {
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

static inline void
jack_midi_reset_frame (midi_frame_t* mf)
{
	mf->len = 0;
	mf->data[0] = 0;
}

/* returns -1 on erroneous frame, 1 if frame is complete, else 0. */
static int
jack_midi_update_frame(uint8_t b, midi_frame_t *mf)
{
	int len;

	if (mf->len == MIDI_FRAME_MAX) {
		/* error, too long frame */
		jack_midi_reset_frame (mf);
		midi_running = 0;
		return (-1);
	}
	if (midi_running != 0 && (b & 0x80) != 0) {
		jack_midi_read_push_back (b);
		midi_running = 0;
		if (mf->len == 0 || ((mf->len - 1) % 2)) {
			jack_midi_reset_frame (mf);
			return (-1);
		}
		else
			return (1);
	}
	if (mf->len == 0) {
		if (b >= 0x80 && b <= 0xef)
			midi_running = b;
		else
			midi_running = 0;
	}

	mf->data[mf->len++] = b;
	len = midi_cmd_len[mf->data[0]];
	if (len == -1) {
		/* error */
		midi_running = 0;
		jack_midi_reset_frame (mf);
		return (-1);
	}
	else if (len == -0xf0 && mf->len > 1) {
		/* system exclusive */
		if (b == 0xf7)
			return (1);
	}
	else if (midi_running == 0 && len == mf->len)
		return (1);

	return (0);
}

static inline int
jack_midi_skip_frame (midi_frame_t *mf)
{
	return (0);
}

static void
jack_midi_dump_frame (int fd, midi_frame_t *mf)
{
	for (int j = 0; j < mf->len; j++)
		dprintf (2, "%x ", mf->data[j]);
}

static void
jack_midi_read_frame()
{
	int r;
	midi_frame_t *current;

	if (midi_frames_len >= MIDI_FRAMES_LEN)
		return;

	jack_midi_lock();
	current = &midi_frames[midi_frames_len];
	r = jack_midi_read_byte(read_fd);
	if (r > 0xff)
		jack_midi_reset_frame (current);
	else if (r >= 0) {
		if (jack_midi_update_frame((uint8_t) r, current) == 1) {
			/* frame is read */
			if ( ! jack_midi_skip_frame (current)) {
				if (debug_mode) {
					dprintf (2, "read frame#%i: ", midi_frames_len);
					jack_midi_dump_frame (2, current);
					dprintf (2, "\n");
				}
				midi_frames_len++;
			}
			else {
				if (debug_mode) {
					dprintf (2, "skipped frame#%i: ", midi_frames_len);
					jack_midi_dump_frame (2, current);
					dprintf (2, "\n");
				}
				jack_midi_reset_frame (current);
			}
		}
	}
	jack_midi_unlock();
}

static void
jack_midi_expand_running (midi_frame_t *mf)
{
	if (mf->data[0] > 0xef || mf->len == 3)
		return;
	else {
		midi_frame_t f;

		memcpy (&f, mf, sizeof (midi_frame_t));
		jack_midi_reset_frame (mf);
		for (int i = 1; i < f.len; i += 2) {
			mf->data[mf->len++] = f.data[0];
			mf->data[mf->len++] = f.data[i];
			mf->data[mf->len++] = f.data[i+1];
		}
	}
}

static void
jack_midi_read(jack_nframes_t nframes)
{
	uint8_t *buffer;
	void *buf;
	midi_frame_t *mf;

	if (midi_frames_len == 0 || midi_frames_offset >= midi_frames_len)
		return;

	jack_midi_lock();

	if (output_port[0] == NULL)
		buf = NULL;
	else {
		buf = jack_port_get_buffer(output_port[0], nframes);
		if (buf != NULL)
			jack_midi_clear_buffer(buf);
		else {
			DPRINTF("jack: cannot send anything "
				"on unit.\n");
		}
	}
	if (buf == NULL)
		DPRINTF("Buffer full. MIDI event lost\n");
	else {
		for (int n = 0; midi_frames_offset < midi_frames_len;
			midi_frames_offset++, n++) {
			mf = &midi_frames[midi_frames_offset];
			if (expand)
				jack_midi_expand_running (mf);
			buffer = jack_midi_event_reserve(buf,
						jack_counter, mf->len);
			if (buffer == NULL)
				break;
			jack_counter++;
			memcpy(buffer, mf->data, mf->len);
			if (debug_mode) {
				dprintf (2, "frame#%i sent to jack: ",
					jack_counter);
				jack_midi_dump_frame (2, mf);
				dprintf (2, "\n");
			}
			jack_midi_reset_frame (mf);
		}
		if (midi_frames_offset == midi_frames_len) {
			midi_frames_len = 0;
			midi_frames_offset = 0;
		}
	}

	jack_midi_unlock();
}

static int
jack_midi_process_callback(jack_nframes_t nframes, void *reserved)
{
	if (nframes) {
		jack_midi_read(nframes);
		jack_midi_write(nframes);
	}

	return (0);
}

static void
jack_midi_jack_shutdown(void *arg)
{
	close(read_fd);
	close(write_fd);
	exit(0);
}

static void
jack_midi_openclose(void)
{
	if (read_name == NULL) {
		/* do nothing */
	}
	else if (read_fd < 0) {
		read_fd = open(read_name, O_RDONLY);
		if (read_fd > -1) {
			jack_midi_lock();
			fcntl(read_fd, F_SETFL, (int)O_NONBLOCK);
			jack_midi_unlock();
		}
	}
	else if (jack_midi_poll(read_fd) < 0) {
		jack_midi_lock();
		DPRINTF("Close read\n");
		close(read_fd);
		read_fd = -1;
		read_offset = 0;
		read_len = 0;
		jack_midi_unlock();
	}

	if (write_name == NULL) {
		/* do nothing */
	}
	else if (write_fd < 0) {
		write_fd = open(write_name, O_WRONLY);
		if (write_fd > -1) {
			jack_midi_lock();
			fcntl(write_fd, F_SETFL, (int)0);
			jack_midi_unlock();
		}
	}
	else if (jack_midi_poll(write_fd) < 0) {
		jack_midi_lock();
		DPRINTF("Close write\n");
		close(write_fd);
		write_fd = -1;
		jack_midi_unlock();
	}

	/* check if we should close */
	if (kill_on_close != 0) {
		int stop = 0;

		if (write_name != NULL && write_fd == -1)
			stop = 1;
		if (read_name != NULL && read_fd == -1)
			stop = 1;
		if (stop)
			jack_midi_jack_shutdown(NULL);
	}
}

static void
usage()
{
	dprintf (2,
	    "jack_midi - Jack MIDI socket client\n"
	    "	-d /dev/midi0.0 set capture and playback device\n"
	    "	-C /dev/midi0.0 set capture device\n"
	    "	-P /dev/midi0.0 set playback device\n"
	    "	-U <username> attach to this JACK user\n"
	    "	-B run in background\n"
	    "	-k terminate client if a device goes away\n"
	    "	-n specify Jack port name: default is jack_midi_...\n"
	    "   -g show trames (debug mode)\n"
	    "	-h (show help)\n");
	exit (0);
}

static void
jack_midi_pipe(int dummy)
{
}

static void
jack_midi_create_client ()
{	
	char *devname;
	int error;

	if (port_name)
		devname = strdup (port_name);
	else {
		const char *pname;
		int len;

		if (read_name != NULL) {
			if (strncmp (read_name, "/dev/", 5) == 0)
				pname = read_name + 5;
			else
				pname = read_name;
		}
		else {
			if (strncmp (write_name, "/dev/", 5) == 0)
				pname = write_name + 5;
			else
				pname = write_name;
		}
		len = strlen (pname) + 2 + strlen (JACK_PORT_NAME);
		devname = malloc (len);
		snprintf (devname, len, "%s_%s", JACK_PORT_NAME, pname);
	}
	if (devname == NULL)
		errx (EX_OSERR, "Out of memory.");

	while (jack_client == NULL) {
		jack_client = jack_client_open (devname,
						JackNoStartServer, NULL);
		if (jack_client == NULL) {
			if (background) {
				/* check status of MIDI device */
				jack_midi_openclose ();

				/* wait a bit */
				usleep(100000);
			}
			else {
				errx (EX_UNAVAILABLE, "Could not connect "
			    		"to the JACK server. Run jackd first?");
			}
		}
	}
	free (devname);
	error = jack_set_process_callback (jack_client,
					jack_midi_process_callback, 0);
	if (error) {
		errx (EX_UNAVAILABLE, "Could not register "
					"JACK process callback.");
	}

	jack_set_buffer_size (jack_client, 64);
	jack_on_shutdown (jack_client, jack_midi_jack_shutdown, 0);

	if (read_name != NULL) {
		output_port[0] = jack_port_register (
		    jack_client, ".TX", JACK_DEFAULT_MIDI_TYPE,
		    JackPortIsOutput | JackPortIsPhysical |
		    JackPortIsTerminal, 0);

		if (output_port[0] == NULL) {
			errx (EX_UNAVAILABLE, "Could not "
			    "register JACK output port.");
		}
	}
	if (write_name != NULL) {

		input_port = jack_port_register (
		    jack_client, ".RX", JACK_DEFAULT_MIDI_TYPE,
		    JackPortIsInput | JackPortIsPhysical |
		    JackPortIsTerminal, 0);

		if (input_port == NULL) {
			errx (EX_UNAVAILABLE, "Could not "
			    "register JACK input port.");
		}
	}
	if (jack_activate (jack_client))
		errx (EX_UNAVAILABLE, "Cannot activate JACK client.");
}

int
main(int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "U:kBd:hP:SC:n:gx")) != -1) {
		switch (c) {
		case 'k':
			kill_on_close = 1;
			break;
		case 'B':
			background = 1;
			break;
		case 'd':
			free(read_name);
			free(write_name);
			read_name = strdup(optarg);
			write_name = strdup(optarg);
			break;
		case 'P':
			free(write_name);
			write_name = strdup(optarg);
			break;
		case 'C':
			free(read_name);
			read_name = strdup(optarg);
			break;
		case 'n':
			free(port_name);
			port_name = strdup(optarg);
			break;
		case 'U':
			jack_midi_uid(optarg);
			break;
		case 'g':
			debug_mode = 1;
			break;
		case 'x':
			expand = 1;
			break;
		case 'h':
		default:
			usage();
		}
	}

	if (read_name == NULL && write_name == NULL)
		usage();

	if (background) {
		if (daemon(0, 0))
			errx(EX_UNAVAILABLE, "Could not become daemon");
	}
	signal(SIGPIPE, jack_midi_pipe);

	pthread_mutex_init(&jack_midi_mtx, NULL);

	/* try to open MIDI device early on */
	jack_midi_openclose();

	if (uid_found != 0 && setuid(uid) != 0)
		errx(EX_UNAVAILABLE, "Could not set user ID");

	/* create jack client */
	jack_midi_create_client ();

	/* loop */
	while (1) {
		/* check status of MIDI device */
		jack_midi_openclose();

		/* read frame */
		jack_midi_read_frame();

		/* wait a bit */
		usleep(1000);
	}

	/* not reached */
	return (0);
}
