# jack_midi

This program connects any raw MIDI device such as /dev/midiX or /dev/umidiX for USB MIDI one to a Jack server.

It should work on any BSDs (tested on FreeBSD 13+). Note that under NetBSD and OpenBSD, MIDI devices are named /dev/rmidiXXX.

## How to build

Jack server must be installed on the system.

Simply run

	make

and then, as root

	make install

## Usage

Ensure you have a running Jack server (jackd) *before* starting this program.

As an example, if you want to connect /dev/midi0.0 to Jack, use:

	jack_midi -d /dev/midi0.0 -B

which will launch it in background mode as a daemon. Jack will now manage the two MIDI ports _midi0.0.TX_ and _midi0.0.RX_. You can also add only a capture (*-C*) or playback (*-P*) device.

See the man page jack_midi.8 for more details. You can dump the MIDI frames (*-g*) or expand running status frames (*-x*).

## Authors

This program is using some code from jack_umidi, which does quite the same thing but only for USB MIDI devices on FreeBSD (by Hans Petter Selasky).

Main author: Nicolas Provost, 2025.

