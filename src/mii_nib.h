/*
 * mii_nib.h
 *
 * Copyright (C) 2024 Michel Pollet <buserror@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "mii_floppy.h"

int
mii_floppy_nib_load(
		mii_floppy_t *f,
		mii_dd_file_t *file );

// Convert a 6656-byte NIB track into the on-disk bitstream for the drive.
// Exposed so platforms without mmap/PSRAM staging can stream-load images.
void
mii_floppy_nib_render_track(
		uint8_t *src_track,
		mii_floppy_track_t *dst,
		uint8_t *dst_track);
void
_mii_floppy_nib_write_sector(
		mii_dd_file_t *file,
		uint8_t *track_data,
		mii_floppy_track_map_t *map,
		uint8_t track_id,
		uint8_t sector,
		uint8_t data_sector[342 + 1] );
