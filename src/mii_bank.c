/*
 * mii_bank.c
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
#include "mii_bank.h"
#include "debug_log.h"


void
mii_bank_init(
		mii_bank_t *bank)
{
	if (bank->raw)
		return;
	if (bank->logical_mem_offset == 0 && !bank->no_alloc) {
		bank->raw = calloc(1, bank->size * 256);
		bank->alloc = 1;
	}
}

void
mii_bank_dispose(
		mii_bank_t *bank)
{
//	printf("%s %s\n", __func__, bank->name);
	if (bank->alloc)
		free(bank->raw);
	bank->raw = NULL;
	bank->alloc = 0;
#if WITH_BANK_ACCESS
	if (bank->access) {
		// Allow callback to free anything it wants
		for (int i = 0; i < bank->size; i++)
			if (bank->access[i].cb)
				bank->access[i].cb(NULL, bank->access[i].param, 0, NULL, false);
		free(bank->access);
	}
	bank->access = NULL;
#endif
}

bool
mii_bank_access(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t *data,
		uint16_t len,
		bool write)
{
#if WITH_BANK_ACCESS
	uint8_t page_index = (addr - bank->base) >> 8;
	if (bank->access && bank->access[page_index].cb) {
		if (bank->access[page_index].cb(bank, bank->access[page_index].param,
					addr, (uint8_t *)data, write))
			return true;
	}
#endif
	return false;
}

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

void
mii_bank_write(
		mii_bank_t *bank,
		uint16_t addr,
		const uint8_t *data,
		uint16_t len)
{
    if (mii_bank_access(bank, addr, data, len, true))
        return;
	if (!bank->vram) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		do {
			bank->raw[phy++] = *data++;
		} while (likely(--len));
		return;
	}
	while (len) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		uint32_t off  = phy & RAM_IN_PAGE_ADDR_MASK;
		uint32_t n    = MIN(len, RAM_PAGE_SIZE - off);
		uint32_t phys_page = get_ram_page_for(bank, phy);
		bank->vram_desc->desc[phys_page].dirty = 1;
		uint8_t *dst = bank->raw + (phys_page << 8) + off;
		memcpy(dst, data, n);
		addr += n;
		data += n;
		len  -= n;
	}
}

void
mii_bank_read(
		mii_bank_t *bank,
		uint16_t addr,
		uint8_t *data,
		uint16_t len)
{
	if (mii_bank_access(bank, addr, data, len, false))
		return;
	if (!bank->vram) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		do {
			*data++ = bank->raw[phy++];
		} while (likely(--len));
		return;
	}
	while (len) {
		uint32_t phy = bank->logical_mem_offset + addr - bank->base;
		uint32_t off  = phy & RAM_IN_PAGE_ADDR_MASK;
		uint32_t n    = MIN(len, RAM_PAGE_SIZE - off);
		uint32_t phys_page = get_ram_page_for(bank, phy);
		uint8_t *dst = bank->raw + (phys_page << 8) + off;
		memcpy(data, dst, n);
		addr += n;
		data += n;
		len  -= n;
	}
}

#if WITH_BANK_ACCESS
void
mii_bank_install_access_cb(
		mii_bank_t *bank,
		mii_bank_access_cb cb,
		void *param,
		uint8_t page,
		uint8_t end)
{
	if (!end)
		end = page;
	if ((page << 8) < bank->base || (end << 8) > (bank->base + bank->size * 256)) {
		MII_DEBUG_PRINTF("%s %s INVALID install access cb %p param %p page %02x-%02x\n",
					__func__, bank->name, cb, param, page, end);
		return;
	}
	page -= bank->base >> 8;
	end -= bank->base >> 8;
	if (!bank->access) {
		bank->access = calloc(1, bank->size * sizeof(bank->access[0]));
	}
	MII_DEBUG_PRINTF("%s %s install access cb page %02x:%02x\n",
			__func__, bank->name, page, end);
	for (int i = page; i <= end; i++) {
		if (bank->access[i].cb)
			MII_DEBUG_PRINTF("%s %s page %02x already has a callback\n",
					__func__, bank->name, i);
		bank->access[i].cb = cb;
		bank->access[i].param = param;
	}
}
#endif
