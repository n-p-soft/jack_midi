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

#include "midi_reader.h"

#define	JACK_PORT_NAME		"jack_midi"
#define	JACK_OUT_MAX	17		/* units */

static jack_port_t *output_port[JACK_OUT_MAX];
static jack_port_t *input_port;
static jack_client_t *jack_client;
static midi_reader_t reader;
static int read_fd = -1;
static int write_fd = -1;
static int kill_on_close;
static int debug_mode;
static char *read_name = NULL;
static char *write_name = NULL;
static char *port_name = NULL;
static pthread_mutex_t jack_midi_mtx; /* protects read_fd, write_fd */
static uid_t uid = -1;
static jack_nframes_t jack_counter;

#ifdef HAVE_DEBUG
#define	DPRINTF(fmt, ...) printf("%s:%d: " fmt, __FUNCTION__, __LINE__,## __VA_ARGS__)
#else
#define	DPRINTF(fmt, ...) do { } while (0)
#endif

#define JACK_MIDI_VERSION "1.01"

static void
jack_midi_lock ()
{
	pthread_mutex_lock(&jack_midi_mtx);
}

static void
jack_midi_unlock ()
{
	pthread_mutex_unlock(&jack_midi_mtx);
}

static void
jack_midi_uid (const char *s)
{
	struct passwd *pw;

	pw = getpwnam (s);
	if (pw == NULL)
		errx (EX_OSERR, "Unknown user");
	else
		uid = pw->pw_uid;
}

static void
jack_midi_write (jack_nframes_t nframes)
{
	int error;
	int events;
	int i;
	void *buf;
	jack_midi_event_t event;

	if (input_port == NULL)
		return;

	buf = jack_port_get_buffer (input_port, nframes);
	if (buf == NULL) {
		DPRINTF ("jack_port_get_buffer() failed, "
				"cannot receive anything.\n");
		return;
	}
#ifdef JACK_MIDI_NEEDS_NFRAMES
	events = jack_midi_get_event_count (buf, nframes);
#else
	events = jack_midi_get_event_count (buf);
#endif

	for (i = 0; i < events; i++) {
#ifdef JACK_MIDI_NEEDS_NFRAMES
		error = jack_midi_event_get(&event, buf, i, nframes);
#else
		error = jack_midi_event_get(&event, buf, i);
#endif
		if (error) {
			DPRINTF ("lost MIDI event.\n");
			continue;
		}
		jack_midi_lock ();
		if (write_fd > -1) {
			if (write (write_fd, event.buffer, event.size) !=
								event.size) {
				DPRINTF ("write() failed.\n");
			}
		}
		jack_midi_unlock ();
	}
}

static void
jack_midi_read (jack_nframes_t nframes)
{
	uint8_t *buffer;
	void *buf;
	midi_frame_t *mf;

	if ( ! midi_reader_update (&reader))
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
		while (1) {
			mf = midi_reader_get_next (&reader);
			if (mf == NULL)
				break;
			buffer = jack_midi_event_reserve(buf,
						jack_counter, mf->len);
			if (buffer == NULL)
				break;
			jack_counter++;
			memcpy(buffer, mf->data, mf->len);
			if (debug_mode) {
				dprintf (2, "frame#%i sent to jack: ",
					jack_counter);
				midi_frame_dump (2, mf);
				dprintf (2, "\n");
			}
		}
	}

	jack_midi_unlock();
}

static int
jack_midi_process_callback (jack_nframes_t nframes, void *reserved)
{
	if (nframes) {
		jack_midi_read (nframes);
		jack_midi_write (nframes);
	}
	return (0);
}

static void
jack_midi_jack_shutdown (void *arg)
{
	midi_reader_close (&reader);
	close (write_fd);
	exit (0);
}

static void
jack_midi_openclose (void)
{
	if (read_name) {
		if (read_fd < 0) {
			read_fd = open (read_name, O_RDONLY | O_NONBLOCK);
			if (read_fd > -1) {
				jack_midi_lock ();
				midi_reader_set_fd (&reader, read_fd);
				jack_midi_unlock ();
			}
		}
		else if (midi_reader_poll (&reader) < 0) {
			DPRINTF ("Close read\n");
			jack_midi_lock ();
			midi_reader_close (&reader);
			read_fd = -1;
			jack_midi_unlock ();
		}
	}

	if (write_name) {
		if (write_fd < 0) {
			jack_midi_lock ();
			write_fd = open (write_name, O_WRONLY | O_NONBLOCK);
			jack_midi_unlock ();
		}
		else if (fcntl (write_fd, F_SETFL, (int) O_NONBLOCK) < 0) {
			DPRINTF ("Close write\n");
			jack_midi_lock ();
			close (write_fd);
			write_fd = -1;
			jack_midi_unlock ();
		}
	}

	/* check if we should close */
	if (kill_on_close != 0) {
		int stop = 0;

		if (write_name != NULL && write_fd == -1)
			stop = 1;
		if (read_name != NULL && read_fd == -1)
			stop = 1;
		if (stop)
			jack_midi_jack_shutdown (NULL);
	}
}

static void
usage (const char *msg)
{
	dprintf (2,
	"jack_midi v%s - Jack MIDI socket client\n"
	"    -d </dev/xxx> set capture and playback device\n"
	"    -C </dev/xxx> set capture device only\n"
	"    -P </dev/xxx> set playback device only\n"
	"    -U <username> attach to this JACK user\n"
	"    -B run in background\n"
	"    -k terminate client if a device goes away\n"
	"    -n <port> specify Jack port name: default is jack_midi_...\n"
	"    -g show trames (debug mode)\n"
	"    -x expand running status MIDI frames\n"
	"    -f <n> filter-out frames with status byte <n>\n"
	"    -m <file> dump frames to <file> (descriptor or path)\n"
	"    -M <file> dump frames to <file> (descriptor or path), hex mode\n"
	"    -h (show help)\n",
	JACK_MIDI_VERSION);
	if (msg)
		dprintf (2, "%s\n", msg);
	exit (msg ? 1 : 0);
}

static void
jack_midi_pipe(int dummy)
{
}

static void
jack_midi_log_callback (const char *desc)
{
}

static void
jack_midi_create_client (int background)
{	
	char *devname;
	int error;

	if (jack_client)
		return;

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

	jack_client = jack_client_open (devname, JackNoStartServer, NULL);
	free (devname);
	if (jack_client == NULL) {
		/* check status of MIDI device */
		jack_midi_openclose ();
	}
	else {
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
}

int
main(int argc, char **argv)
{
	int c;
	int have_uid = 0;
	int expand = 0;
	int background = 0;
	midi_reader_flags_t flags = 0;
	unsigned char to_skip[256];
	int skipped = 0;
	char *endptr;
	long l;
	char *dump_file = NULL;
	int dump_hex = 0;
	int start = 1;

	to_skip[0] = 0;
	while ((c = getopt(argc, argv, "U:kBd:hP:SC:n:gxf:m:M:")) != -1) {
		switch (c) {
		case 'k':
			kill_on_close = 1;
			break;
		case 'B':
			background = 1;
			break;
		case 'd':
			free (read_name);
			free (write_name);
			read_name = strdup (optarg);
			write_name = strdup (optarg);
			break;
		case 'P':
			free (write_name);
			write_name = strdup (optarg);
			break;
		case 'C':
			free (read_name);
			read_name = strdup (optarg);
			break;
		case 'n':
			free (port_name);
			port_name = strdup (optarg);
			break;
		case 'U':
			jack_midi_uid (optarg);
			have_uid = 1;
			break;
		case 'g':
			debug_mode = 1;
			break;
		case 'x':
			expand = 1;
			break;
		case 'f':
			if (skipped == 254) {
				errx (EX_USAGE,
					"too many skipped status bytes.");
			}
			l = strtol (optarg, &endptr, 0);
			if (l < 0 || l > 255 || (endptr && *endptr)) {
				errx (EX_USAGE, "bad argument for -f (%s)",
					optarg);
			}
			to_skip[skipped++] = (unsigned char) l;
			break;
		case 'M':
			dump_hex = 1;
			/* fallthru */
		case 'm':
			free (dump_file);
			dump_file = strdup (optarg);
			break;
		case 'h':
			usage (NULL);
			break;
		default:
			usage ("Unknown option.");
		}
	}

	if ((read_name == NULL && write_name == NULL) ||
		(dump_file != NULL && read_name == NULL))
		usage ("Missing device path.");

	if (background) {
		if (daemon (0, 0))
			errx (EX_UNAVAILABLE, "Could not become daemon");
	}

	if (have_uid && setuid (uid) != 0)
		errx (EX_UNAVAILABLE, "Could not set user ID");

	signal (SIGPIPE, jack_midi_pipe);

	/* MIDI reader setup */
	if (debug_mode)
		flags += MIDIR_DEBUG;
	if (expand)
		flags += MIDIR_EXPAND;
	if (dump_hex)
		flags += MIDIR_DUMPHEX;
	midi_reader_init (&reader, flags, skipped ? to_skip : NULL);
	if (dump_file) {
		int dfd;

		if (dump_file[0] >= '0' && dump_file[0] <= '9') {
			l = strtol (dump_file, &endptr, 0);
			if (l < 0 || l > 255 || (endptr && *endptr))
				errx (EX_USAGE, "bad dump file descriptor");
			else
				dfd = (int) l;
		}
		else {
			dfd = open (dump_file, O_CREAT|O_TRUNC|O_WRONLY, 0600);
			if (dfd < 0) {
				errx (EX_OSERR, "unable to open file %s",
					dump_file);
			}
		}
		free (dump_file);
		midi_reader_set_dump_fd (&reader, dfd);
	}

	pthread_mutex_init (&jack_midi_mtx, NULL);
	jack_error_callback = jack_midi_log_callback;
	jack_info_callback = jack_midi_log_callback;

	/* loop */
	while (1) {
		/* check status of MIDI device */
		jack_midi_openclose ();

		/* create jack client if needed */
		jack_midi_create_client (background);
		if (jack_client == NULL) {
			if (dump_file == NULL) {
				errx (EX_UNAVAILABLE,
				"Unable to create Jack client and no dump file "
				"file requested, stopping now. Check that a "
				" Jack server is running.");
			}
			else if (start) {
				start = 0;
				warnx ("Unable to create Jack client; dump "
					"mode only until a Jack server is "
					"started.");
			}
		}

		/* read frame */
		midi_reader_update (&reader);
		if (jack_client == NULL)
			midi_reader_clear_queue (&reader);

		/* wait a bit */
		usleep(1000);
	}

	/* not reached */
	return (0);
}
