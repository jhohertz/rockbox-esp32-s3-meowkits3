/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__  _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2026 by the Rockbox project
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Storage on the MeowKit: the SDHOST block is driven by ESP-IDF's own driver.
 *
 * The bare-metal branch drove this controller by hand, through Rockbox's
 * generic protocol layer in drivers/sdmmc_host.c. That code is in git but not
 * here, and the reason is worth recording because the failure was not obvious.
 * Booted under ESP-IDF, the hand-driven path accepts every command, the start
 * bit clears, and then nothing ever completes: MINTSTS stays at zero, not even
 * for a response timeout. A CIU whose command clock was never switched on and a
 * card that never answers look identical from the status registers, and the
 * first reads as "no card in the slot".
 *
 * Every register that path writes has now been matched against IDF's own
 * sequence -- same clock source (PLL_F160M) and dividers, same card-clock
 * enable with the low-power gating bit left clear, same GPIO matrix routing,
 * clock and reset taken through periph_module_enable/reset rather than by
 * poking SYSTEM_PERIPHS_CLK_EN1 -- and the block still stays silent, while
 * IDF's driver reads the same card on the same pins in the same image.
 *
 * So the protocol layer goes too: it exists to sit on top of the ops this
 * target used to supply. What Rockbox actually needs from a storage device is
 * the sd_* entry points, and those are thin over IDF's sdmmc_host / sdmmc_cmd
 * API. IDF's driver owns the GPIO matrix, the clock tree, the IDMAC descriptor
 * ring, its interrupt, and the cache maintenance a DMA engine needs when the
 * destination is in PSRAM -- all of which the hand-driven path had to reproduce
 * and none of which it reproduced correctly.
 */

/* IDF first, and not for tidiness. Rockbox's kernel has a queue.h and so does
 * FreeRTOS, both guarded by QUEUE_H, and whichever is seen first owns that name
 * for the whole translation unit. If Rockbox's goes first, FreeRTOS.h's own
 * `#include <queue.h>` is skipped by the guard and every FreeRTOS type after it
 * comes out unknown. This file uses neither queue API, so it can take whichever
 * one arrives first. */
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_err.h"
#include "esp_log.h"

#include <errno.h>

#include "config.h"
#include "system.h"
#include "file.h"
#include "storage.h"
#include "sdmmc.h"
#include "debug.h"

/* Board wiring, from the reference firmware: SDMMC slot 0, 1-bit.
 * CLK 47, CMD 48, D0 21. There is no card-detect and no write-protect line, so
 * the medium is not detectable and the driver must not pretend otherwise. */
#define SD_CLK_PIN  47
#define SD_CMD_PIN  48
#define SD_D0_PIN   21

static const char *TAG = "rbsd";

static sdmmc_host_t host = SDMMC_HOST_DEFAULT();
static sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
static sdmmc_card_t card;
static tCardInfo cardinfo;
static volatile long sd_last_activity;
static bool sd_ready;

int sd_init(void)
{
    if (sd_ready)
        return 0;

    /* Only the 1-bit flag: the board wires D0 and no other data line. Leaving
     * the wider capabilities in place lets the card negotiate a bus width that
     * has no copper behind it. */
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.slot = SDMMC_HOST_SLOT_1;
    /* 20 MHz. The slot could run at 40 MHz (IDF's SDMMC_FREQ_HIGHSPEED) from
     * the 160 MHz generator, but the lines are on the GPIO matrix with no
     * delay-phase tuning here, and the reference firmware does not push past
     * that either. */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    slot.clk = SD_CLK_PIN;
    slot.cmd = SD_CMD_PIN;
    slot.d0 = SD_D0_PIN;
    slot.cd = -1;
    slot.wp = -1;
    slot.width = 1;

    esp_err_t err = sdmmc_host_init();
    /* Stamped either way, not only on success: the trail is how a crash inside the
     * first card access after recording starts gets attributed, and a step that
     * never returns leaves its completion note absent. */
    xtensa_breadcrumb("sdHost", (unsigned long)err, 0);
    if (err == ESP_OK)
        err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    xtensa_breadcrumb("sdSlot", (unsigned long)err, 0);
    if (err == ESP_OK)
        err = sdmmc_card_init(&host, &card);
    xtensa_breadcrumb("sdCard", (unsigned long)err, 0);

    if (err != ESP_OK)
    {
        DEBUGF("SD init failed: %s", esp_err_to_name(err));
        return EIO;
    }

    /* Only the two fields Rockbox actually reads. tCardInfo carries CSD detail
     * (taac, r2w_factor, current_bank) that the generic SD protocol layer used to
     * fill in while it was negotiating the card; here the negotiation belongs to
     * IDF and nothing in the tree asks for the rest. */
    cardinfo.numblocks = card.csd.capacity;
    cardinfo.blocksize = card.csd.sector_size;

    sd_ready = true;
    ESP_LOGI(TAG, "SD card up: %d x %d byte sectors, %d MHz, %d-bit",
             card.csd.capacity, card.csd.sector_size, host.max_freq_khz, slot.width);

    return 0;
}

tCardInfo *card_get_info_target(int drive)
{
    (void)drive;
    return &cardinfo;
}

int sd_read_sectors(IF_MD(int drive,) sector_t start, int count, void *buf)
{
    (void)IF_MD_DRV(drive);
    if (!sd_ready)
        return EIO;

    sd_last_activity = current_tick;

    {
        esp_err_t e;

        xtensa_breadcrumb("sdRd", (unsigned long)start, (unsigned long)count);
        e = sdmmc_read_sectors(&card, buf, (uint32_t)start,
                               (uint32_t)count);
        xtensa_breadcrumb("sdRc", (unsigned long)e, (unsigned long)count);
        return e == ESP_OK ? 0 : EIO;
    }
}

int sd_write_sectors(IF_MD(int drive,) sector_t start, int count, const void *buf)
{
    (void)IF_MD_DRV(drive);
    if (!sd_ready)
        return EIO;

    sd_last_activity = current_tick;

    {
        esp_err_t e;

        /* A recording write is the access most likely to be running when the
         * recorder faults, so both edges are marked: "sdWr" present with no
         * "sdWc" means the call itself did not return. */
        xtensa_breadcrumb("sdWr", (unsigned long)start, (unsigned long)count);
        e = sdmmc_write_sectors(&card, buf, (uint32_t)start,
                                (uint32_t)count);
        xtensa_breadcrumb("sdWc", (unsigned long)e, (unsigned long)count);
        return e == ESP_OK ? 0 : EIO;
    }
}

/* Card power on this board is an AXP173 rail, not a slot switch, and IDF's host
 * has no "idle but resident" state to drop into without tearing the slot down.
 * Nothing in Rockbox needs the card unpowered while the player runs, so this is
 * deliberately a no-op rather than a deinit that the next access would have to
 * undo. */
void sd_enable(bool enabled)
{
    (void)enabled;
}

long sd_last_disk_activity(void)
{
    return sd_last_activity;
}

int sd_event(long id, intptr_t data)
{
    return storage_event_default_handler(id, data, sd_last_activity, STORAGE_SD);
}
