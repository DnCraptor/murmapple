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
	for (int i = 0; i < 4; i++)
		a->v[i].value = 127;
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
			 * No need starting the cycle timers when nobody cares about
			 * the analog values aka joysticks
			 */
			if (!a->enabled) {
				a->enabled = true;
				printf("ANALOG: enabling paddle timers (first $C070 strobe)\n");
				/*
				 * No need for a function pointer here for the timer, the
				 * decrementing value is just what we need, and we're quite
				 * happy to stop at ~0 as well.
				 */
				for (int i = 0; i < 4; i++)
					a->v[i].timer_id = mii_timer_register(mii,
									NULL, NULL, 0, __func__);
			}
			/*
			 * Multiplying by mii->speed allows reading joystick in
			 * 'fast' emulation mode, this basically simulate slowing down
			 * just for the joystick reading
			 */
			for (int i = 0; i < 4; i++) {
				int64_t timer_val = ((a->v[i].value * 11) * mii->speed);
				mii_timer_set(mii, a->v[i].timer_id, timer_val);
			}
			// Debug: print first few times we strobe
			static int strobe_count = 0;
			if (strobe_count < 5) {
				printf("ANALOG: $C070 strobe #%d values=%d,%d,%d,%d speed=%.2f\n",
					strobe_count, a->v[0].value, a->v[1].value, 
					a->v[2].value, a->v[3].value, mii->speed);
				strobe_count++;
			}
		}	break;
		case 0xc064 ... 0xc067: {
			int idx = addr - 0xc064;
			// Debug: warn if reading before $C070 strobe
			if (!a->enabled) {
				static int warn_count = 0;
				if (warn_count < 5) {
					printf("ANALOG: WARNING reading PDL%d before $C070 strobe! timer_id=%d\n", 
						idx, a->v[idx].timer_id);
					warn_count++;
				}
				// Return 0x00 (no delay) since timers not initialized
				*byte = 0x00;
				break;
			}
			*byte = mii_timer_get(mii, a->v[idx].timer_id) > 0 ? 0x80 : 0x00;
		}	break;
	}
}

