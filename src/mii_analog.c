/*
 * mii_analog.c
 *
 * Copyright (C) 2023 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mii.h"
#include "mii_analog.h"

/*
 * Analog joystick
 * This is fairly easy, as long as the 65c02 respects the proper cycle
 * count for all the instruction involved in reading, as it's very cycle
 * sensitive.
 * the UI fills up the analog values in mii_t, and here we just simulate
 * the capacitor decay.
 */

void
mii_analog_init(
		struct mii_t *mii,
		mii_analog_t * a )
{
	memset(a, 0, sizeof(*a));
	// Default to center position (127) - games use paddle timers for delays
	// and value=0 means timer=0 cycles = no delay at all!
	for (int i = 0; i < 4; i++) {
		a->v[i].value = 127;
		a->v[i].timer_id = 0xFF;  // Invalid until registered
	}
	
	// Pre-register paddle timers so they're ready when the game strobes $C070
	// These timers have no callback - they just count down from the set value
	for (int i = 0; i < 4; i++) {
		a->v[i].timer_id = mii_timer_register(mii, NULL, NULL, 0, "analog");
	}
	a->enabled = true;  // Enable immediately so first strobe works
}

/*
 * https://retrocomputing.stackexchange.com/questions/15093/how-do-i-read-the-position-of-an-apple-ii-joystick
 */
void
mii_analog_access(
		mii_t *mii,
		mii_analog_t * a,
		uint16_t addr,
		uint8_t * byte,
		bool write)
{
	if (write)
		return;
	switch (addr) {
		case 0xc070: {
			/*
			 * Multiplying by mii->speed allows reading joystick in
			 * 'fast' emulation mode, this basically simulate slowing down
			 * just for the joystick reading
			 */
			for (int i = 0; i < 4; i++) {
				int64_t timer_val = ((a->v[i].value * 11) * mii->speed);
				mii_timer_set(mii, a->v[i].timer_id, timer_val);
			}
		}	break;
		case 0xc064 ... 0xc067: {
			int idx = addr - 0xc064;
			// Timer > 0 means still counting down, return 0x80
			// Timer <= 0 means expired, return 0x00
			int64_t t = mii_timer_get(mii, a->v[idx].timer_id);
			*byte = t > 0 ? 0x80 : 0x00;
		}	break;
	}
}

