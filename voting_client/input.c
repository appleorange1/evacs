/* This file is (C) copyright 2001-2004 Software Improvements, Pty Ltd */

/* This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <signal.h>
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "child_barcode.h"
#include "input.h"
#include "audio.h"
#include "image.h"

/* SIPL 2011-07-06 This input driver now supports three types of keypad:
   1. The "original" numeric keypads
   2. The AEC keypads (custom-built for the 2007 Federal Election)
   3. Targus keypads (custom-modified with new keypad overlays).

   This file must be compiled twice, to obtain .o files which go into
   two separate binaries:
   A. Compile without additional options to get support for
      keypad styles 1 & 2.
   B. Compile with -DTARGUS_KEYPAD to get support for keypad style 3.

   (Because there are (incompatible) overlaps between the keycodes
   generated by keypad styles 1 & 3, it is not possible to support
   both keypad styles in the same binary.  The script which invokes
   the voting client must first detect whether or not a style 3 keypad
   is attached, and invoke the corresponding version of the binary.)
*/

/* Keycodes which form reset keys */
/* SIPL 2011-06-09 Support original keypad and new Targus
   telephone-style keypad. */
#ifdef TARGUS_KEYPAD
static int reset_key[] = { 0x53 /*7*/, 0x50 /*5*/, 0x3f /*3*/ };
#else
static int reset_key[] = { 0x54 /*5*/, 0x51 /*9*/, 0x57 /*1*/ };
#endif

/* static means this starts at all false */
static bool reset_key_status[sizeof(reset_key)/sizeof(reset_key[0])];

/* Keycodes which form shutdown keys */
/* SIPL 2011-06-09 Support original keypad and new Targus
   telephone-style keypad. */
#ifdef TARGUS_KEYPAD
static int shutdown_key[] = { 0x17 /*1*/, 0x50 /*5*/, 0x55 /*9*/ };
#else
static int shutdown_key[] = { 0x54 /*5*/, 0x4F /*7*/, 0x59 /*3*/ };
#endif

/* static means this starts at all false */
static bool shutdown_key_status[sizeof(reset_key)/sizeof(reset_key[0])];

/* The barcode-reading child */
static pid_t child;

/* The UNIX pipe they will send the barcode down */
static int pipe_from_child;

struct keymap
{
	unsigned int xkey;
	enum input_event evacs_key;
};

/* SIPL 2011-06-01 Modified to work with evdev keyboard driver. */
/* SIPL 2011-06-09 Support original keypad and new Targus
   telephone-style keypad. */
#ifdef TARGUS_KEYPAD
struct keymap keymap[] = {
	{0x17, INPUT_START_AGAIN},
	{0x6a, INPUT_UP},
	{0x4f, INPUT_PREVIOUS},
	{0x54, INPUT_DOWN},
	{0x51, INPUT_NEXT},
	{0x50, INPUT_SELECT},
	{0x57, INPUT_UNDO},
	{0x59, INPUT_FINISH},
	{0x53, INPUT_VOLUME_DOWN},
	{0x55, INPUT_VOLUME_UP}
};
#else
struct keymap keymap[] = {
	/* Original keypad */
	{0x4d, INPUT_START_AGAIN},
	{0x50, INPUT_UP},
	{0x53, INPUT_PREVIOUS},
	{0x58, INPUT_DOWN},
	{0x55, INPUT_NEXT},
	{0x5a, INPUT_SELECT},
	{0x16, INPUT_UNDO},
	{0x68, INPUT_FINISH},
	{0x6a, INPUT_VOLUME_DOWN},
	{0x3f, INPUT_VOLUME_UP}
};

/* 2011-07-06 Now support the original keypad and the custom-built AEC
   keypads.  They produce completely different keycodes, but the "*"
   "#" keys are problematic:  the "*" key generates Shift-8, and
   the "#" key generates Shift-3.  Handle all this by remapping
   the keycodes generated by the AEC keypad to the keycodes generated
   by the original keypad.
*/
struct keymap_remap
{
	unsigned int xkey_original; /* Keycode generated by AEC keypad */
	unsigned int xkey_new; /* Map to this keycode generated by
				  the original keypad, if the shift key is
				  not depressed. */
	unsigned int xkey_new_with_shift; /* Map to this keycode generated by
				  the original keypad, if the shift key is
				  depressed. */
};
struct keymap_remap aec_to_original[] = {
	/* AEC BVI trial keypad.  Note the two rows which have
	   different values for xkey_new and xkey_new_with_shift,
	   i.e., "8"/"*" and "3"/"#".
	   Even though the "0" key is not used in voting, it is
	   used in the reset/shutdown sequences, so it must appear
	   in this list.
	*/
	{0x0a, 0x4d, 0x4d}, /* INPUT_START_AGAIN */
	{0x0b, 0x50, 0x50}, /* INPUT_UP */
	{0x0d, 0x53, 0x53}, /* INPUT_PREVIOUS */
	{0x0e, 0x5a, 0x5a}, /* INPUT_SELECT */
	{0x0f, 0x55, 0x55}, /* INPUT_NEXT */
	{0x10, 0x6a, 0x6a}, /* INPUT_VOLUME_DOWN */
	{0x11, 0x58, 0x16}, /* INPUT_DOWN, INPUT_UNDO */
	{0x12, 0x3f, 0x3f}, /* INPUT_VOLUME_UP */
	{0x0c, 0x59, 0x68}, /* "3", INPUT_FINISH */
	{0x13, 0x13, 0x13}  /* "0" (Used only in reset/shutdown) */
};
#endif

void do_reset(void)
{
	/* Terminate the child */
	kill(child, SIGTERM);

	/* Clean up the audio device */
	audio_shutdown();

	/* Close the display */
	close_display();

	/* Exit the program */
	exit(0);
}

static void do_shutdown(void)
{
	/* Terminate the child */
	kill(child, SIGTERM);

	/* Clean up the audio device */
	audio_shutdown();

	/* Close the display */
	close_display();

	/* Issue a shutdown */
        /* Use "set -m" to turn on job control in the shell,
           so that shutdown, which is started as a background process,
           is not killed when this process calls exit().
           By default, job control is off in bash when called
           from system() (which invokes sh -c "..."). */
	system("set -m; shutdown -h -t 3 now &");

	/* Exit the program */
	exit(0);
}

#ifndef TARGUS_KEYPAD
/* Because it is impossible to generate simultaneous keypresses on the
   AEC keypad, store the most recent keypresses, then compare those
   with reset and shutdown _sequences_. */

/* If you change the length of either of these arrays, make sure to
   adjust the length of the most_recent_keys array in store_aec_key()
   as appropriate. */
static const unsigned int reset_key_sequence[] =
	{ 0x59 /* 3 */, 0x59 /* 3 */, 0x59 /* 3 */,
	  0x13 /* 0 */, 0x13 /* 0 */,
	  0x59 /* 3 */, 0x59 /* 3 */, 0x59 /* 3 */ };
static const unsigned int reset_key_sequence_length =
	sizeof(reset_key_sequence)/sizeof(unsigned int);

static const unsigned int shutdown_key_sequence[] =
	{ 0x13 /* 0 */, 0x13 /* 0 */, 0x13 /* 0 */,
	  0x59 /* 3 */, 0x59 /* 3 */,
	  0x13 /* 0 */, 0x13 /* 0 */, 0x13 /* 0 */ };
static const unsigned int shutdown_key_sequence_length =
	sizeof(shutdown_key_sequence)/sizeof(unsigned int);

/* SIPL 2011-06-07 New function.  If a keycode is detected which
   must have emanated from an AEC keypad, call this function to
   register it.  Then detect if the program should do the reset
   or shutdown sequences.  Since each key generates both KeyPress
   and KeyRelease events, call this function only after receiving
   (say) KeyPress events to avoid duplicates.
*/
static void store_aec_key(unsigned int last_keycode)
{
	/* Make this array the same length at least as long as the
	   longer of the two reset_key_sequence and
	   shutdown_key_sequence arrays.
	*/
	static unsigned int most_recent_keys[] = {0, 0, 0, 0, 0, 0, 0, 0};
	static const unsigned int most_recent_keys_length =
		sizeof(most_recent_keys)/sizeof(unsigned int);
	/* i iterates over most_recent_keys; j iterates
	   over both reset_key_sequence and shutdown_key_sequence
	   in turn. */
	int i, j;

	for (i = 0; i < most_recent_keys_length - 1; i++)
		/* Shift left */
		most_recent_keys[i] = most_recent_keys[i+1];

	/* Store last_keycode in the last place. */
	most_recent_keys[most_recent_keys_length - 1] = last_keycode;

	/* Now check against reset_key_sequence.
	   The calculation of the initial value of i allows for
	   the possibility that reset_key_sequence is a shorter
	   array than most_recent_keys.
	 */
	for (i = most_recent_keys_length
		     - reset_key_sequence_length, j = 0;
	     j < reset_key_sequence_length;
	     i++, j++)
		if (most_recent_keys[i] != reset_key_sequence[j])
			break;
	if (j == reset_key_sequence_length)
		/* Matched against the whole length of the sequence. */
		do_reset();

	/* Now check against shutdown_key_sequence.
	   The calculation of the initial value of i allows for
	   the possibility that shutdown_key_sequence is a shorter
	   array than most_recent_keys.
	*/
	for (i = most_recent_keys_length
		     - shutdown_key_sequence_length, j = 0;
	     j < shutdown_key_sequence_length;
	     i++, j++)
		if (most_recent_keys[i] != shutdown_key_sequence[j])
			break;
	if (j == shutdown_key_sequence_length)
		/* Matched against the whole length of the sequence. */
		do_shutdown();
}
#endif

static XEvent get_event(void)
{
	XEvent event;
	unsigned int i;

	XNextEvent(get_display(), &event);

#ifndef TARGUS_KEYPAD
	/* SIPL 2011-07-06 Manage the AEC keypad by first checking:
	   (a) Is the event a KeyPress or KeyRelease?
	     (a.i) If so, is the key one of the keys specific
	       to the AEC keypad?
	       (a.i.1) If so, is the shift key being held down?
		  (a.i.1.a) If so, alter the keycode to the corresponding
		    xkey_new_with_shift value.
		  (a.i.1.b) Else, alter the keycode to the corresponding
		    xkey_new value.
	*/
	if ((event.type == KeyPress) || (event.type == KeyRelease)) {
	    /* Now check if it is an AEC-keypad-specific keycode. */
	    for (i = 0; i < sizeof(aec_to_original)/
			 sizeof(aec_to_original[0]); i++) {
		if (aec_to_original[i].xkey_original ==
		    event.xkey.keycode) {
		    /* It is an AEC-keypad-specific keycode. */
		    /* First, ensure the reset and shutdown key sequences
		       are modified to the versions for this keypad. */
		    /* set_reset_and_shutdown_keys_for_aec(); */
		    /* Now, remap the keycode. */
		    if (event.xkey.state & ShiftMask) {
			event.xkey.keycode =
				aec_to_original[i].xkey_new_with_shift;
		    } else {
			event.xkey.keycode =
				aec_to_original[i].xkey_new;
		    }
		    /* Now, store the mapped keycode. Only register KeyPress
		       events. */
		    if (event.type == KeyPress)
			    store_aec_key(event.xkey.keycode);
		}
	    }
	}

	/* Now, almost all traces of the AEC keypad are gone.
	   Only the keycode for the left shift key remains, and
	   that is dealt with by get_keystroke_or_barcode().
	   Otherwise, the keycodes have been replaced with the
	   corresponding keycodes of the original keypads.
	 */
#endif

	/* Do processing of reset events here, in common code. */
	if (event.type == KeyRelease) {
		/* If it's a reset key, mark it false */
		for (i = 0; i < sizeof(reset_key)/sizeof(reset_key[0]); i++) {
			if (reset_key[i] == event.xkey.keycode)
				reset_key_status[i] = false;
		}
		/* If it's a shutdown key, mark it false */
		for (i = 0; i < sizeof(shutdown_key)/sizeof(shutdown_key[0]); i++) {
			if (shutdown_key[i] == event.xkey.keycode)
				shutdown_key_status[i] = false;
		}
	} else if (event.type == KeyPress) {
		/* If it's a reset key, mark it true */
		for (i = 0; i < sizeof(reset_key)/sizeof(reset_key[0]); i++) {
			if (reset_key[i] == event.xkey.keycode)
				reset_key_status[i] = true;
		}

		/* Are they all true? */
		for (i = 0; reset_key_status[i] == true; i++) {
			if (i == sizeof(reset_key)/sizeof(reset_key[0])-1)
				do_reset();
		}
		/* If it's a shutdown key, mark it true */
		for (i = 0; i < sizeof(shutdown_key)/sizeof(shutdown_key[0]); i++) {
			if (shutdown_key[i] == event.xkey.keycode)
				shutdown_key_status[i] = true;
		}

		/* Are they all true? */
		for (i = 0; shutdown_key_status[i] == true; i++) {
			if (i == sizeof(shutdown_key)/sizeof(shutdown_key[0])-1)
				do_shutdown();
		}
	}

	return event;
}

/* Return the map for a give X keycode */
static enum input_event map_key(unsigned int keycode)
{
	unsigned int i;

	for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
		if (keymap[i].xkey == keycode)
			return keymap[i].evacs_key;
	}
	/* An unused key was pressed */
	return INPUT_UNUSED;
}

/* DDS???: Read Barcode */
static enum input_event read_barcode(struct barcode *bc)
{
	int n;

	/* Child will have written nul-terminated barcode to the pipe */
	n = read(pipe_from_child, bc->ascii, sizeof(bc->ascii));
	assert(n > 0);

	/* Make sure it's terminated. */
	bc->ascii[sizeof(bc->ascii)-1] = '\0';

	return INPUT_BARCODE;
}

/* Waiting for either a keypress or a barcode (in which case,
   bc->ascii filled in). */
enum input_event get_keystroke_or_barcode(struct barcode *bc)
{
	XEvent event;

	/* Loop until we get a relevent keypress, or client message. */
	do {
		event = get_event();
		if (event.type == ClientMessage)
			return read_barcode(bc);
#ifdef TARGUS_KEYPAD
		/* SIPL 2011-06-10
		   For the Targus keypad, ignore a NumLock key event
		   (received before and after some
		   of the keys). Set the event type to KeyRelease to force
		   the loop to go round again.
		*/
#define NUMLOCK_KEYCODE 77
		if ((event.type == KeyPress) &&
		    (event.xkey.keycode == NUMLOCK_KEYCODE))
			event.type = KeyRelease;
# else
		/* SIPL 2011-07-06
		   Support AEC keypad, which sends "left shift"
		   before and after the "*" and "#" keys.
		   Ignore those events.
		 */
#define SHIFT_L_KEYCODE 50
		if ((event.type == KeyPress) &&
		    (event.xkey.keycode == SHIFT_L_KEYCODE))
			event.type = KeyRelease;
#endif
	} while (event.type != KeyPress);

	return map_key(event.xkey.keycode);
}

/* DDS???: Get Keystroke */
enum input_event get_keystroke(void)
{
	enum input_event event;

	/* We want to loop here reading the barcode, so they aren't
           buffered */
	do {
		struct barcode dummy;
		event = get_keystroke_or_barcode(&dummy);
	} while (event == INPUT_BARCODE);

	return event;
}

/* Wait for end of the world */
void wait_for_reset(void)
{
	/* get_event handles RESET internally */
	while (true) get_event();
}

bool initialize_input(void)
{
	/* We fork, and child reads the serial.  It sends us a barcode
	   through the pipe, and notifies us via a synthetic X event */
	int pipeend[2];
	struct pollfd pollset;
	char dummy;

	if (pipe(pipeend) != 0)
		return false;

	child = fork();
	/* Did fork fail? */
	if (child == (pid_t)-1) {
		close(pipeend[0]);
		close(pipeend[1]);
		return false;
	}

	/* Are we the child? */
	if (child == 0) {
		close(pipeend[0]);
		child_barcode(pipeend[1]);
	}

	close(pipeend[1]);
	pipe_from_child = pipeend[0];

	/* Give kiddy 5 seconds to acknowledge. */
	pollset.fd = pipe_from_child;
	pollset.events = POLL_IN;

	/* If we timeout, or the read fails, return false. */
	if (poll(&pollset, 1, 5000) != 1
	    || read(pipe_from_child, &dummy, 1) != 1) {
		close(pipe_from_child);
		return false;
	}

	return true;
}
