/*
 * mii_aeram.c
 *
 * Accurate Slinky/RamFactor-compatible AE RAM (4MB) emulation:
 * - Slot ROM signatures ($Cs00..$Cs07, $CsFA/$CsFB, $CsFF)
 * - ProDOS firmware entry via ZP $42..$47 (status/read/write block)
 * - Protocol Converter entry (cmd $00..$09, errors exactly)
 * - Slinky registers at C080..C083, C08F with auto-increment
 * - Partition table at card addr 000000..0000FF + screen holes updates
 * - Registers disabled after reset; enabled upon any access to $Cs00..$CsFF
 *
 * Timing is not emulated (fast responses are OK).
 *
 * SPDX-License-Identifier: MIT
 */
#if PICO_RP2350

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mii.h"
#include "mii_bank.h"
#include "mii_slot.h"
#include "debug_log.h"

#include "disk_loader.h"
#include "../drivers/psram_allocator.h"

#define AE_RAM_BASE (PSRAM_DATA + BDSK_BYTES)

// ---------------------------- Config ----------------------------

#define AE_RAM_BYTES     (4u * 1024u * 1024u)
#define AE_PAGE_SIZE     256u
#define AE_BLOCK_SIZE    512u

#define AE_TOTAL_PAGES   (AE_RAM_BYTES / AE_PAGE_SIZE)
#define AE_TOTAL_BLOCKS  (AE_RAM_BYTES / AE_BLOCK_SIZE)

// Firmware reserves 1024 bytes when partition manager has been used. :contentReference[oaicite:17]{index=17}
// Keep pages 0..3 reserved, and place partition table at page 0.
#define AE_RESERVED_BYTES 1024u
#define AE_RESERVED_PAGES (AE_RESERVED_BYTES / AE_PAGE_SIZE)

// Slot ROM offsets
#define AE_PRODOS_OFF   0x45
#define AE_PC_OFF       (AE_PRODOS_OFF + 3)

#define AE_ROM_SIG0     0xC9
#define AE_ROM_SIG1     0x20

// ---------------------------- Helpers you must implement ----------------------------
// For Protocol Converter cmd $08/$09, bit23 selects Main/Aux buffer. :contentReference[oaicite:18]{index=18}
// In your emulator, buffer bank selection is derived from mii->mem[page].read/write.
// Here we need a way to force MainMem vs AuxMem independent of current soft-switches.
//
// Provide these two helpers using your existing main/aux banking model.
//
// Return bank pointer that will write INTO Apple memory at [buffer..buffer+len)
// (i.e., equivalent of current "RAMWRT = main/aux").
static inline mii_bank_t*
ae_get_bank_for_buffer_write(mii_t *mii, uint16_t buffer, bool buffer_is_aux)
{
    (void)buffer;

    if (buffer_is_aux) {
        // Принудительно AUX RAM (RamWorks banked)
        return &mii->bank[MII_BANK_AUX];
    } else {
        // Принудительно MAIN RAM
        return &mii->bank[MII_BANK_MAIN];
    }
}

// Return bank pointer that will read FROM Apple memory at [buffer..buffer+len)
static inline mii_bank_t*
ae_get_bank_for_buffer_read(mii_t *mii, uint16_t buffer, bool buffer_is_aux)
{
    (void)buffer;

    if (buffer_is_aux) {
        return &mii->bank[MII_BANK_AUX];
    } else {
        return &mii->bank[MII_BANK_MAIN];
    }
}

// ---------------------------- Slot ROM (Cs00..CsFF) ----------------------------

static const uint8_t mii_rom_aeram[256] = {
    // $Cs00..$Cs07 signature for Apple MemExp/RamFactor :contentReference[oaicite:21]{index=21}
    0xC9,0x20, 0xC9,0x00, 0xC9,0x03, 0xC9,0x00,

    [0x08 ... 0xF9] = 0xEA,

    // Brand and flags :contentReference[oaicite:22]{index=22}
    [0xFA] = 0xAE, // RamFactor id (we keep AE for compatibility)
    [0xFB] = 0x01, // bit0=1; RamFactor uses 0x01

    [0xFC] = 0xEA,
    [0xFD] = 0xEA,
    [0xFE] = 0xEA,

    // $CsFF = ProDOS entry offset, PC entry = +3 :contentReference[oaicite:23]{index=23}
    [0xFF] = AE_PRODOS_OFF,
};

// ---------------------------- Screen holes + partition table ----------------------------

static inline void
ae_write_card_u8(mii_card_aeram_t *c, uint32_t card_addr, uint8_t v)
{
    if (card_addr < AE_RAM_BYTES) AE_RAM_BASE[card_addr] = v;
}

static inline uint8_t
ae_read_card_u8(mii_card_aeram_t *c, uint32_t card_addr)
{
    if (card_addr < AE_RAM_BYTES) return AE_RAM_BASE[card_addr];
    return 0x00;
}

static void
ae_build_default_partitions(mii_card_aeram_t *c)
{
    // 9 partitions; all memory except 1024 bytes in partition 1, others empty :contentReference[oaicite:24]{index=24}
    memset(c->part, 0, sizeof(c->part));
    c->partitioned = true;
    c->current_part = 1;

    const uint32_t usable_pages = AE_TOTAL_PAGES - AE_RESERVED_PAGES;

    c->part[0].base_page = AE_RESERVED_PAGES;
    c->part[0].size_pages = usable_pages;
    c->part[0].os_code = 0;
    c->part[0].os_check = 0;
    memcpy(c->part[0].name, "RAMCARD         ", 16);

    for (int i = 1; i < 9; i++) {
        c->part[i].base_page = c->part[0].base_page + c->part[0].size_pages; // empty at end
        c->part[i].size_pages = 0;
        c->part[i].os_code = 0;
        c->part[i].os_check = 0;
        memcpy(c->part[i].name, "                ", 16);
    }

    // Write partition table at card addr 000000..0000FF :contentReference[oaicite:25]{index=25}
    // Layout: 00:$AE, 01:$F4, 02:index, 03:index^$5A, 04: blocks/256 entire card :contentReference[oaicite:26]{index=26}
    ae_write_card_u8(c, 0x000000, 0xAE);
    ae_write_card_u8(c, 0x000001, 0xF4);

    uint8_t part_index = (uint8_t)(24 * c->current_part - 16); // part# in 1..9 :contentReference[oaicite:27]{index=27}
    ae_write_card_u8(c, 0x000002, part_index);
    ae_write_card_u8(c, 0x000003, (uint8_t)(part_index ^ 0x5A));

    ae_write_card_u8(c, 0x000004, (uint8_t)(AE_TOTAL_BLOCKS / 256)); // blocks/256 :contentReference[oaicite:28]{index=28}

    // Partition descriptors: 9 groups * 24 bytes starting at 0x08 :contentReference[oaicite:29]{index=29}
    for (int p = 0; p < 9; p++) {
        const uint32_t off = 0x08 + (uint32_t)p * 24;

        // base address hi/mid (page-based; low implied 0) :contentReference[oaicite:30]{index=30}
        uint32_t base = c->part[p].base_page;
        uint32_t size = c->part[p].size_pages;

        ae_write_card_u8(c, off + 0, (uint8_t)((base >> 8) & 0xFF)); // hi
        ae_write_card_u8(c, off + 1, (uint8_t)((base >> 0) & 0xFF)); // mid (page low)
        ae_write_card_u8(c, off + 2, (uint8_t)((size >> 8) & 0xFF)); // hi
        ae_write_card_u8(c, off + 3, (uint8_t)((size >> 0) & 0xFF)); // mid
        ae_write_card_u8(c, off + 4, c->part[p].os_code);
        ae_write_card_u8(c, off + 5, c->part[p].os_check);

        for (int i = 0; i < 16; i++)
            ae_write_card_u8(c, off + 8 + i, (uint8_t)c->part[p].name[i]);
    }
}

static void
ae_update_screenholes(mii_t *mii, mii_card_aeram_t *c)
{
    // Only valid after accessing a partition :contentReference[oaicite:31]{index=31}
    const int slot = c->slot->id + 1;
    const uint16_t sh478 = (uint16_t)(0x0478 + slot);
    const uint16_t sh4F8 = (uint16_t)(0x04F8 + slot);
    const uint16_t sh578 = (uint16_t)(0x0578 + slot);
    const uint16_t sh5F8 = (uint16_t)(0x05F8 + slot);
    const uint16_t sh678 = (uint16_t)(0x0678 + slot);
    const uint16_t sh6F8 = (uint16_t)(0x06F8 + slot);
    const uint16_t sh778 = (uint16_t)(0x0778 + slot);
    const uint16_t sh7F8 = (uint16_t)(0x07F8 + slot);

    const ae_part_t *p = &c->part[c->current_part - 1];

    // Entire card size in blocks/256 at $478+slot :contentReference[oaicite:32]{index=32}
    mii_write_one(mii, sh478, (uint8_t)(AE_TOTAL_BLOCKS / 256));

    // partition index pointer at $4F8+slot :contentReference[oaicite:33]{index=33}
    mii_write_one(mii, sh4F8, (uint8_t)(24 * c->current_part - 16));

    // partition base address hi/mid at $578/$5F8 :contentReference[oaicite:34]{index=34}
    mii_write_one(mii, sh578, (uint8_t)((p->base_page >> 8) & 0xFF));
    mii_write_one(mii, sh5F8, (uint8_t)((p->base_page >> 0) & 0xFF));

    // #pages hi/lo at $678/$6F8; pages/2 = blocks :contentReference[oaicite:35]{index=35}
    mii_write_one(mii, sh678, (uint8_t)((p->size_pages >> 8) & 0xFF));
    mii_write_one(mii, sh6F8, (uint8_t)((p->size_pages >> 0) & 0xFF));

    mii_write_one(mii, sh778, p->os_code);
    mii_write_one(mii, sh7F8, p->os_check);
}

static inline uint32_t
ae_part_blocks(const ae_part_t *p)
{
    return (p->size_pages / 2u);
}

static inline uint32_t
ae_part_base_bytes(const ae_part_t *p)
{
    return p->base_page * AE_PAGE_SIZE;
}

// ---------------------------- Slinky register address + auto-increment ----------------------------

static inline uint32_t
ae_get_card_addr24(mii_card_aeram_t *c)
{
    return ((uint32_t)c->addr_h << 16) | ((uint32_t)c->addr_m << 8) | (uint32_t)c->addr_l;
}

static inline void
ae_set_card_addr24(mii_card_aeram_t *c, uint32_t a)
{
    c->addr_l = (uint8_t)(a & 0xFF);
    c->addr_m = (uint8_t)((a >> 8) & 0xFF);
    c->addr_h = (uint8_t)((a >> 16) & 0xFF);
}

static inline void
ae_inc_card_addr24(mii_card_aeram_t *c)
{
    uint32_t a = ae_get_card_addr24(c);
    a = (a + 1u) & 0x00FFFFFFu;
    ae_set_card_addr24(c, a);
}

// ---------------------------- ProDOS firmware entry (ZP $42..$47 ABI) ----------------------------
// The manual explicitly shows using $42..$47 and firmware entry point to call status/read/write. :contentReference[oaicite:36]{index=36}
// For our device, implement command 0/1/2 consistent with SmartPort-like ZP ABI.

static void
_mii_aeram_prodos_callback(mii_t *mii, uint8_t trap)
{
    (void)trap;
    int sid = ((mii->cpu.PC >> 8) & 0xF) - 1;
    mii_card_aeram_t *c = mii->slot[sid].drv_priv;

    if (!c->partitioned)
        ae_build_default_partitions(c);
    ae_update_screenholes(mii, c);

    uint8_t  command  = mii_read_one(mii, 0x42);
    uint8_t  unit     = mii_read_one(mii, 0x43);
    uint16_t buffer   = mii_read_word(mii, 0x44);
    uint16_t blk16    = mii_read_word(mii, 0x46);

    // unit bit7 may be used as in other firmwares; keep compatibility.
    if (unit & 0x80) unit >>= 7;

    // We expose a single block device as "unit 0/1" tolerant; real PC uses unit 1 for RAM. :contentReference[oaicite:37]{index=37}
    if (unit > 1) { mii->cpu.P.C = 1; return; }

    ae_part_t *p = &c->part[c->current_part - 1];
    uint32_t blocks = ae_part_blocks(p);

    switch (command) {
        case 0: { // status: X/Y = #blocks, carry clear
            mii->cpu.X = (uint8_t)(blocks & 0xFF);
            mii->cpu.Y = (uint8_t)((blocks >> 8) & 0xFF);
            mii->cpu.P.C = 0;
        } break;

        case 1: { // read block
            uint32_t blk = (uint32_t)blk16;
            if (blk >= blocks) { mii->cpu.P.C = 1; break; }

            uint32_t off = ae_part_base_bytes(p) + blk * AE_BLOCK_SIZE;
            mii_bank_t *bank = &mii->bank[mii->mem[buffer >> 8].write];
            mii_bank_write(bank, buffer, AE_RAM_BASE + off, AE_BLOCK_SIZE);
            mii->cpu.P.C = 0;
        } break;

        case 2: { // write block
            uint32_t blk = (uint32_t)blk16;
            if (blk >= blocks) { mii->cpu.P.C = 1; break; }

            uint32_t off = ae_part_base_bytes(p) + blk * AE_BLOCK_SIZE;
            mii_bank_t *bank = &mii->bank[mii->mem[buffer >> 8].read];
            mii_bank_read(bank, buffer, AE_RAM_BASE + off, AE_BLOCK_SIZE);
            mii->cpu.P.C = 0;
        } break;

        default:
            mii->cpu.P.C = 1;
            break;
    }
}

// ---------------------------- Protocol Converter entry ----------------------------

static void
_mii_aeram_pc_callback(mii_t *mii, uint8_t trap)
{
    (void)trap;
    int sid = ((mii->cpu.PC >> 8) & 0xF) - 1;
    mii_card_aeram_t *c = mii->slot[sid].drv_priv;

    if (!c->partitioned)
        ae_build_default_partitions(c);
    ae_update_screenholes(mii, c);

    // Decode JSR inline call format: cmd byte + params pointer word :contentReference[oaicite:38]{index=38}
    uint16_t sp = 0x100 + mii->cpu.S + 1;
    uint16_t call_addr = mii_read_word(mii, sp);

    uint8_t  cmd    = mii_read_one(mii, call_addr + 1);
    uint16_t params = mii_read_word(mii, call_addr + 2);

    // return to next byte after pointer
    call_addr += 3;
    mii_write_word(mii, sp, call_addr);

    uint8_t  pcount = mii_read_one(mii, params + 0);
    uint8_t  unit   = mii_read_one(mii, params + 1);
    uint16_t buffer = mii_read_word(mii, params + 2);

    #define ERR(code) do { mii->cpu.P.C = 1; mii->cpu.A = (uint8_t)(code); return; } while(0)
    #define OK()      do { mii->cpu.P.C = 0; mii->cpu.A = 0; } while(0)

    // Units: 0 = PC status, 1 = RAM status/device :contentReference[oaicite:39]{index=39}
    if (unit > 1) ERR(0x11);

    ae_part_t *p = &c->part[c->current_part - 1];
    uint32_t part_blocks = ae_part_blocks(p);

    switch (cmd) {
        case 0x00: { // Status :contentReference[oaicite:40]{index=40}
            if (pcount != 3) ERR(0x04);
            uint8_t code = mii_read_one(mii, params + 4);

            if (unit == 0) { // PC Status, code must be 0 :contentReference[oaicite:41]{index=41}
                if (code != 0x00) ERR(0x21);
                uint8_t st[8] = { 0x01,0,0,0,0,0,0,0 };
                mii_bank_t *bw = ae_get_bank_for_buffer_write(mii, buffer, false);
                mii_bank_write(bw, buffer, st, 8);
                mii->cpu.X = 0x08;
                mii->cpu.Y = 0x00;
                OK();
                return;
            }

            // RAM Status, code 0 or 3 :contentReference[oaicite:42]{index=42}
            if (code == 0x00) {
                uint8_t st[4];
                st[0] = 0xF8;
                st[1] = (uint8_t)(part_blocks & 0xFF);
                st[2] = (uint8_t)((part_blocks >> 8) & 0xFF);
                st[3] = (uint8_t)((part_blocks >> 16) & 0xFF);

                mii_bank_t *bw = ae_get_bank_for_buffer_write(mii, buffer, false);
                mii_bank_write(bw, buffer, st, 4);
                mii->cpu.X = 0x04;
                mii->cpu.Y = 0x00;
                OK();
                return;
            }
            if (code == 0x03) {
                uint8_t st[25] = {0};
                st[0] = 0xF8;
                st[1] = (uint8_t)(part_blocks & 0xFF);
                st[2] = (uint8_t)((part_blocks >> 8) & 0xFF);
                st[3] = (uint8_t)((part_blocks >> 16) & 0xFF);

                // ProDOS Volume Name format: len + chars + pad blanks :contentReference[oaicite:43]{index=43}
                const char *name = "RAMCARD";
                st[4] = 7;
                for (int i = 0; i < 16; i++) st[5+i] = ' ';
                for (int i = 0; i < 7; i++)  st[5+i] = (uint8_t)name[i];

                mii_bank_t *bw = ae_get_bank_for_buffer_write(mii, buffer, false);
                mii_bank_write(bw, buffer, st, 25);
                mii->cpu.X = 0x19;
                mii->cpu.Y = 0x00;
                OK();
                return;
            }
            ERR(0x21);
        }

        case 0x01: { // Read Block :contentReference[oaicite:44]{index=44}
            if (pcount != 3) ERR(0x04);
            if (unit != 1) ERR(0x11);

            uint32_t blk =
                (uint32_t)mii_read_one(mii, params + 4) |
                ((uint32_t)mii_read_one(mii, params + 5) << 8) |
                ((uint32_t)mii_read_one(mii, params + 6) << 16);

            if (blk >= part_blocks) ERR(0x2D);

            uint32_t off = ae_part_base_bytes(p) + blk * AE_BLOCK_SIZE;

            mii_bank_t *bw = ae_get_bank_for_buffer_write(mii, buffer, false);
            mii_bank_write(bw, buffer, AE_RAM_BASE + off, AE_BLOCK_SIZE);

            OK();
            return;
        }

        case 0x02: { // Write Block :contentReference[oaicite:45]{index=45}
            if (pcount != 3) ERR(0x04);
            if (unit != 1) ERR(0x11);

            uint32_t blk =
                (uint32_t)mii_read_one(mii, params + 4) |
                ((uint32_t)mii_read_one(mii, params + 5) << 8) |
                ((uint32_t)mii_read_one(mii, params + 6) << 16);

            if (blk >= part_blocks) ERR(0x2D);

            uint32_t off = ae_part_base_bytes(p) + blk * AE_BLOCK_SIZE;

            mii_bank_t *br = ae_get_bank_for_buffer_read(mii, buffer, false);
            mii_bank_read(br, buffer, AE_RAM_BASE + off, AE_BLOCK_SIZE);

            OK();
            return;
        }

        case 0x03: { // Format: does nothing :contentReference[oaicite:46]{index=46}
            if (pcount != 1) ERR(0x04);
            if (unit != 1) ERR(0x11);
            OK();
            return;
        }

        case 0x04: { // Control: does nothing; code must be 0 else $21; buffer not touched :contentReference[oaicite:47]{index=47}
            if (pcount != 3) ERR(0x04);
            uint8_t code = mii_read_one(mii, params + 4);
            if (code != 0x00) ERR(0x21);
            OK();
            return;
        }

        case 0x05: { // Init: does nothing :contentReference[oaicite:48]{index=48}
            if (pcount != 1) ERR(0x04);
            OK();
            return;
        }

        case 0x06:
        case 0x07:
            // Open/Close must return error $01 (unsupported cmd class) :contentReference[oaicite:49]{index=49}
            ERR(0x01);

        case 0x08: // Read Bytes :contentReference[oaicite:50]{index=50}
        case 0x09: { // Write Bytes :contentReference[oaicite:51]{index=51}
            if (pcount != 4) ERR(0x04);
            if (unit != 1) ERR(0x11);

            uint16_t cnt =
                (uint16_t)mii_read_one(mii, params + 4) |
                ((uint16_t)mii_read_one(mii, params + 5) << 8);

            uint8_t adr_lo = mii_read_one(mii, params + 6);
            uint8_t adr_mi = mii_read_one(mii, params + 7);
            uint8_t adr_hi = mii_read_one(mii, params + 8);

            // bit23 = buffer main/aux selector; address uses only 0x7FFFFF :contentReference[oaicite:52]{index=52}
            bool buf_aux = (adr_hi & 0x80) != 0;
            uint32_t adr = ((uint32_t)(adr_hi & 0x7F) << 16) | ((uint32_t)adr_mi << 8) | adr_lo;

            if (adr + cnt > AE_RAM_BYTES) ERR(0x2D);

            if (cmd == 0x08) {
                mii_bank_t *bw = ae_get_bank_for_buffer_write(mii, buffer, buf_aux);
                mii_bank_write(bw, buffer, AE_RAM_BASE + adr, cnt);
            } else {
                mii_bank_t *br = ae_get_bank_for_buffer_read(mii, buffer, buf_aux);
                mii_bank_read(br, buffer, AE_RAM_BASE + adr, cnt);
            }
            OK();
            return;
        }

        default:
            // Command not supported :contentReference[oaicite:53]{index=53}
            ERR(0x01);
    }
}

// ---------------------------- Bank access callback to enable registers on Csxx access ----------------------------

#if WITH_BANK_ACCESS
static bool
_ae_slotrom_access_cb(struct mii_bank_t *bank, void *param, uint16_t addr, uint8_t *byte, bool write)
{
    (void)bank; (void)addr; (void)byte; (void)write;
    mii_card_aeram_t *c = (mii_card_aeram_t*)param;
    // Any access to $Cs00..$CsFF enables registers :contentReference[oaicite:54]{index=54}
    c->regs_enabled = true;
    return false; // do not intercept ROM content
}
#endif

// ---------------------------- Slot IO access (C08x regs) ----------------------------

static uint8_t
_mii_aeram_access(mii_t *mii, struct mii_slot_t *slot,
                  uint16_t addr, uint8_t byte, bool write)
{
    (void)mii;
    mii_card_aeram_t *c = slot->drv_priv;
    if (!c) return 0;

    // decode register within slot IO block: C0n0..C0nF, we care about 0/1/2/3/F
    uint8_t reg = (uint8_t)(addr & 0x0F);

    // Registers are disabled after reset until any Csxx access :contentReference[oaicite:55]{index=55}
    if (!c->regs_enabled) {
        // real card: reads may return open bus; we just return 0
        return 0;
    }

    switch (reg) {
        case 0x0: // ADDRL
            if (write) c->addr_l = byte;
            return c->addr_l;

        case 0x1: // ADDRM
            if (write) c->addr_m = byte;
            return c->addr_m;

        case 0x2: // ADDRH
            if (write) c->addr_h = byte;
            return c->addr_h;

        case 0x3: { // DATA at addressed location, auto-inc :contentReference[oaicite:56]{index=56}
            uint32_t a = ae_get_card_addr24(c);
            uint8_t v = 0;
            if (write) {
                if (a < AE_RAM_BYTES) AE_RAM_BASE[a] = byte;
                ae_inc_card_addr24(c);
                return 0;
            } else {
                if (a < AE_RAM_BYTES) v = AE_RAM_BASE[a];
                ae_inc_card_addr24(c);
                return v;
            }
        }

        case 0xF: // Firmware Bank Select :contentReference[oaicite:57]{index=57}
            if (write) c->fw_bank = byte;
            return c->fw_bank;

        default:
            return 0;
    }
}

// ---------------------------- Slot init/dispose ----------------------------

static int
_mii_aeram_init(mii_t *mii, struct mii_slot_t *slot)
{
    static mii_card_aeram_t _c; // keep static like your smartport driver
    mii_card_aeram_t *c = &_c;

    memset(c, 0, sizeof(*c));
    c->slot = slot;
    slot->drv_priv = c;

    memset(AE_RAM_BASE, 0, AE_RAM_BYTES);

    c->regs_enabled = false;
    c->fw_bank = 0;

    // install slot ROM page at C100 + slot*0x100
    uint16_t rom_base = (uint16_t)(0xC100 + slot->id * 0x100);
    mii_bank_write(&mii->bank[MII_BANK_CARD_ROM], rom_base, mii_rom_aeram, sizeof(mii_rom_aeram));

    // patch traps into ProDOS and PC entries
    uint8_t trap_prodos = mii_register_trap(mii, _mii_aeram_prodos_callback);
    uint8_t trap_pc     = mii_register_trap(mii, _mii_aeram_pc_callback);

    uint8_t stub_prodos[2] = { trap_prodos, 0x60 }; // trap; RTS
    uint8_t stub_pc[2]     = { trap_pc,     0x60 }; // trap; RTS

    mii_bank_write(&mii->bank[MII_BANK_CARD_ROM], rom_base + AE_PRODOS_OFF, stub_prodos, sizeof(stub_prodos));
    mii_bank_write(&mii->bank[MII_BANK_CARD_ROM], rom_base + AE_PC_OFF,     stub_pc,     sizeof(stub_pc));

#if WITH_BANK_ACCESS
    // Enable-on-ROM-access hook: install callback for this slot ROM page :contentReference[oaicite:58]{index=58}
    mii_bank_install_access_cb(&mii->bank[MII_BANK_CARD_ROM], _ae_slotrom_access_cb, c,
                              (uint8_t)(rom_base >> 8),
                              (uint8_t)(rom_base >> 8));
#endif

    // Build initial partition table so utilities see it immediately.
    ae_build_default_partitions(c);

    return 0;
}

static void
_mii_aeram_dispose(mii_t *mii, struct mii_slot_t *slot)
{
    (void)mii;
    mii_card_aeram_t *c = slot->drv_priv;
    if (!c) return;
    // If your psram allocator has free, call it. Otherwise keep it persistent.
    // psram_free(c->ram);
    slot->drv_priv = NULL;
}

static int
_mii_aeram_command(mii_t *mii, struct mii_slot_t *slot, uint32_t cmd, void *param)
{
    (void)mii; (void)slot; (void)cmd; (void)param;
    return -1;
}

static mii_slot_drv_t _driver = {
    .name    = "aeram4m",
    .desc    = "AE RAM 4MB",
    .init    = _mii_aeram_init,
    .dispose = _mii_aeram_dispose,
    .access  = _mii_aeram_access,
    .command = _mii_aeram_command,
};
MI_DRIVER_REGISTER(_driver);

#endif
