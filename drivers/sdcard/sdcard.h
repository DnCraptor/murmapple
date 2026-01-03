#ifndef _SDCARD_H_
#define _SDCARD_H_

#include "board_config.h"

/* SPI pin assignment from board_config.h
 * M1: CLK=2, CMD=3, DAT0=4, DAT3=5
 * M2: CLK=6, CMD=7, DAT0=4, DAT3=5
 */

#ifndef SDCARD_SPI_BUS
#ifdef BOARD_M2
#define SDCARD_SPI_BUS spi0  // Pins 4-7 can use spi0
#else
#define SDCARD_SPI_BUS spi0  // Pins 2-5 can use spi0
#endif
#endif

#ifndef SDCARD_PIN_SPI0_CS
#define SDCARD_PIN_SPI0_CS     SDCARD_PIN_D3   // DAT3/CS
#endif

#ifndef SDCARD_PIN_SPI0_SCK
#define SDCARD_PIN_SPI0_SCK    SDCARD_PIN_CLK  // CLK
#endif

#ifndef SDCARD_PIN_SPI0_MOSI
#define SDCARD_PIN_SPI0_MOSI   SDCARD_PIN_CMD  // CMD/MOSI
#endif

#ifndef SDCARD_PIN_SPI0_MISO 
#define SDCARD_PIN_SPI0_MISO   SDCARD_PIN_D0   // DAT0/MISO
#endif

#endif // _SDCARD_H_
