#ifndef PS2KBD_WRAPPER_H
#define PS2KBD_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2kbd_init(void);
void ps2kbd_tick(void);
int ps2kbd_get_key(int* pressed, unsigned char* key);
uint8_t ps2kbd_get_modifiers(void);
uint8_t ps2kbd_get_arrow_state(void);  // bits: 0=right, 1=left, 2=down, 3=up

#ifdef __cplusplus
}
#endif

#endif
