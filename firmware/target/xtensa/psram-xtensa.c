/***************************************************************************
 * ESP32-S3 octal PSRAM bring-up.
 *
 * The ESP32-S3 has no external RAM controller in the sense the rest of Rockbox
 * expects. The 32 MB window at 0x3C000000 is the MSPI (SPI0) "external memory"
 * view, and a 512-entry MMU decides per 64 KB page whether that page is flash or
 * PSRAM. The chip itself has to be told to speak octal before any of that works.
 *
 * Neither the mask ROM nor ESP-IDF's second-stage bootloader does this: ESP-IDF
 * does it in the application, in esp_psram_impl_octal.c. Rockbox is the
 * application, so the bring-up has to happen here, before mmu_init() maps the
 * pages the audiobuffer, framebuffer and codec buffers live in.
 *
 * The device is the 8 MB octal PSRAM on the WROOM-1-N16R8 module. It hangs off
 * SPI1 - the second instance of the same hardware block, 0x1000 below SPI0 - on
 * CS1, and is reached through SPI0's Ext_RAM path once that path's command,
 * address and dummy phases are configured for octal mode.
 *
 * The ROM does export esp_rom_opiflash_exec_cmd(), but it takes thirteen
 * arguments. The ROM is windowed-ABI and Rockbox is call0, so a trampoline would
 * have to place arguments in physical registers the caller cannot name: a
 * windowed callee at window+4 receives its arguments in physical a6..a19, and
 * call0 code can only name a0..a15. Programming the transaction directly is
 * shorter and clearer, and the ROM does nothing more than what is below.
 *
 * Register offsets and bit positions are from
 * components/soc/esp32s3/register/soc/spi_mem_reg.h.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "xtensa.h"
#include "gpio-xtensa.h"
#include "psram-xtensa.h"

#ifdef CPU_XTENSA

/* SPI_MEM instances: 0 is SPI0 (the cache/MMU path), 1 is SPI1. */
#define SPI_MEM_BASE(i)       (0x60003000u - (unsigned)(i) * 0x1000u)

#define SPI_CMD(i)            (SPI_MEM_BASE(i) + 0x00)
#define SPI_ADDR(i)           (SPI_MEM_BASE(i) + 0x04)
#define SPI_CTRL(i)           (SPI_MEM_BASE(i) + 0x08)
#define SPI_CLOCK(i)          (SPI_MEM_BASE(i) + 0x14)
#define SPI_USER(i)           (SPI_MEM_BASE(i) + 0x18)
#define SPI_USER1(i)          (SPI_MEM_BASE(i) + 0x1C)
#define SPI_USER2(i)          (SPI_MEM_BASE(i) + 0x20)
#define SPI_MOSI_DLEN(i)      (SPI_MEM_BASE(i) + 0x24)
#define SPI_MISO_DLEN(i)      (SPI_MEM_BASE(i) + 0x28)
#define SPI_MISC(i)           (SPI_MEM_BASE(i) + 0x34)
#define SPI_CACHE_SCTRL(i)    (SPI_MEM_BASE(i) + 0x40)
#define SPI_SRAM_CMD(i)       (SPI_MEM_BASE(i) + 0x44)
#define SPI_SRAM_DRD(i)       (SPI_MEM_BASE(i) + 0x48)
#define SPI_SRAM_DWR(i)       (SPI_MEM_BASE(i) + 0x4C)
#define SPI_SRAM_CLK(i)       (SPI_MEM_BASE(i) + 0x50)
#define SPI_W0(i)             (SPI_MEM_BASE(i) + 0x58)
#define SPI_CORE_CLK_SEL(i)   (SPI_MEM_BASE(i) + 0xEC)
#define SPI_SMEM_AC(i)        (SPI_MEM_BASE(i) + 0xDC)
#define SPI_FMEM_DDR(i)       (SPI_MEM_BASE(i) + 0xE0)
#define SPI_SMEM_DDR(i)       (SPI_MEM_BASE(i) + 0xE4)
#define SPI_DATE(i)           (SPI_MEM_BASE(i) + 0x3FC)

#define CMD_USR               (1u << 18)

/* CTRL_REG: the hardware "fast read" modes and the per-phase octal enables. */
#define CTRL_FREAD_QIO      (1u << 24)
#define CTRL_FREAD_DIO      (1u << 23)
#define CTRL_FREAD_QUAD     (1u << 20)
#define CTRL_FREAD_DUAL     (1u << 14)
#define CTRL_FCMD_OCT       (1u << 9)
#define CTRL_FADDR_OCT      (1u << 6)
#define CTRL_FDIN_OCT       (1u << 5)
#define CTRL_FDOUT_OCT      (1u << 4)
#define CTRL_FDUMMY_OUT     (1u << 3)

#define USER_COMMAND        (1u << 31)
#define USER_ADDR           (1u << 30)
#define USER_DUMMY          (1u << 29)
#define USER_MISO           (1u << 28)
#define USER_MOSI           (1u << 27)

#define MISC_CS0_DIS        (1u << 0)
#define MISC_CS1_DIS        (1u << 1)

/* SPI0's Ext_RAM (PSRAM) path. */
#define SCTRL_SCMD_4BYTE    (1u << 0)
#define SCTRL_USR_WR_DUMMY  (1u << 3)
#define SCTRL_USR_RD_DUMMY  (1u << 4)
#define SCTRL_USR_RCMD      (1u << 5)
#define SCTRL_USR_WCMD      (1u << 20)
#define SCTRL_SRAM_OCT      (1u << 21)
#define SCMD_SDIN_OCT       (1u << 18)
#define SCMD_SDOUT_OCT      (1u << 19)
#define SCMD_SADDR_OCT      (1u << 20)
#define SCMD_SCMD_OCT       (1u << 21)
#define SCMD_SDUMMY_OUT     (1u << 22)

#define DDR_EN              (1u << 0)
#define DDR_VAR_DUMMY       (1u << 1)
#define DDR_RDAT_SWP        (1u << 2)
#define DDR_WDAT_SWP        (1u << 3)

#define AC_CS_SETUP         (1u << 0)
#define AC_CS_HOLD          (1u << 1)
#define AC_CS_HOLD_TIME_V   0x1F
#define AC_CS_HOLD_TIME_S   7
#define AC_CS_SETUP_TIME_V  0x1F
#define AC_CS_SETUP_TIME_S  2
#define AC_CS_HOLD_DELAY_V  0x3F
#define AC_CS_HOLD_DELAY_S  25

/* OPI PSRAM command words. The command phase is 16 bits because octal mode
 * transfers 8 bits per clock cycle, so a single-line command would waste a
 * cycle. */
#define OPI_SYNC_READ       0x0000
#define OPI_SYNC_WRITE      0x8080
#define OPI_REG_READ        0x4040
#define OPI_REG_WRITE       0xC0C0

#define CMD_BITS            16
#define ADDR_BITS           32
#define RD_DUMMY_BITS       18      /* chip read latency, in dummy-phase bits */
#define WR_DUMMY_BITS       8
#define MR_DUMMY_BITS       8

/* Pad 26 is the SPICS1 pad in the IO MUX; the PSRAM chip select is not routed
 * through the GPIO matrix. */
#define IO_MUX_SPICS1       (DR_REG_IO_MUX_BASE + 0x6C)
#define IO_MUX_FUN_DRV_S    10

#define SYSCON_BASE         0x60026000
#define SYSCON_SRAM_ACE0    (SYSCON_BASE + 0x58)
#define SRAM_ECC_ENABLE     (1u << 8)

/* The MSPI core clock is 160 MHz (CORE_CLK_SEL 2), which is what ESP-IDF uses
 * once it has finished tuning. SPI1 runs at 20 MHz while the chip's mode
 * registers are being programmed; the flash path runs at 80 MHz, the maximum the
 * QSPI flash on the module is rated for; the PSRAM data path runs at 40 MHz.
 * ESP-IDF tunes PSRAM up to 80 MHz by sweeping dummy and delay values, which
 * Rockbox cannot do. 40 MHz is comfortably inside the chip's rating and still
 * several times the bandwidth audio playback needs. */
#define FLASH_DIV           2      /* 80 MHz */
#define BRINGUP_DIV         8      /* 20 MHz */
#define PSRAM_DIV           4      /* 40 MHz */

static bool s_psram_present = false;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)reg = v; }

static void set_spi_clock(int spi, unsigned div)
{
    if (div <= 1)
        wr(SPI_CLOCK(spi), 1u << 31);          /* CLK_EQU_SYSCLK */
    else
        wr(SPI_CLOCK(spi), ((div - 1) << 16) | ((div / 2 - 1) << 8) | (div - 1));
}

static void set_psram_clock(unsigned div)
{
    if (div <= 1)
        wr(SPI_SRAM_CLK(0), 1u << 31);         /* SCLK_EQU_SYSCLK */
    else
        wr(SPI_SRAM_CLK(0), ((div - 1) << 16) | ((div / 2 - 1) << 8) | (div - 1));
}

/* One octal transaction on SPI1: CMD phase, ADDR phase, DUMMY phase, then
 * optionally DOUT and DIN. The data buffer is a little-endian byte array, so the
 * first byte to go out is in bits 0-7 of W0. */
static bool opi_command(uint32_t cmd, int cmd_bits, uint32_t addr, int addr_bits,
                        int dummy_bits, uint32_t dout, int dout_bits,
                        uint32_t *din, int din_bits)
{
    const int spi = 1;
    int guard = 0;

    /* The controller is idle when CMD reads back zero. */
    while (rd(SPI_CMD(spi)) != 0 && guard++ < 10000)
        ;
    if (rd(SPI_CMD(spi)) != 0)
        return false;

    wr(SPI_CTRL(spi), (rd(SPI_CTRL(spi)) &
                       ~(CTRL_FREAD_QIO | CTRL_FREAD_DIO |
                         CTRL_FREAD_QUAD | CTRL_FREAD_DUAL)) |
                      (CTRL_FCMD_OCT | CTRL_FADDR_OCT | CTRL_FDIN_OCT |
                       CTRL_FDOUT_OCT | CTRL_FDUMMY_OUT));

    /* CS1 is the PSRAM, CS0 is the flash. */
    wr(SPI_MISC(spi), (rd(SPI_MISC(spi)) & ~(MISC_CS0_DIS | MISC_CS1_DIS)) |
                      MISC_CS0_DIS);

    wr(SPI_ADDR(spi), addr);
    wr(SPI_USER1(spi), ((addr_bits - 1) << 26) | (dummy_bits > 0 ? dummy_bits - 1 : 0));
    wr(SPI_USER2(spi), ((cmd_bits - 1) << 28) | cmd);
    wr(SPI_USER(spi), USER_COMMAND |
                      (addr_bits ? USER_ADDR : 0) |
                      (dummy_bits ? USER_DUMMY : 0) |
                      (dout_bits ? USER_MOSI : 0) |
                      (din_bits ? USER_MISO : 0));
    wr(SPI_W0(spi), dout);
    wr(SPI_MOSI_DLEN(spi), dout_bits > 0 ? dout_bits - 1 : 0);
    wr(SPI_MISO_DLEN(spi), din_bits > 0 ? din_bits - 1 : 0);

    wr(SPI_CMD(spi), CMD_USR);

    guard = 0;
    while (rd(SPI_CMD(spi)) != 0 && guard++ < 100000)
        ;
    if (rd(SPI_CMD(spi)) != 0)
        return false;

    if (din)
        *din = rd(SPI_W0(spi));

    return true;
}

static void init_psram_pins(void)
{
    /* CS1, HD, WP, CLK, Q0-Q3, D0-D3 all live on the IO MUX, so only the pad
     * function and drive strength need touching. ESP-IDF uses the strongest
     * driver on every one of them. */
    wr(IO_MUX_SPICS1, (rd(IO_MUX_SPICS1) & ~((7u << 12) | (3u << 10))) | (3u << 10));

    for (int pin = 27; pin <= 37; pin++)
        wr(IO_MUX_PIN(pin), (rd(IO_MUX_PIN(pin)) & ~(3u << 10)) | (3u << 10));

    /* The SPI clock pad's drive strength is controlled from the SPI block
     * itself, not the IO MUX. */
    wr(SPI_DATE(0), rd(SPI_DATE(0)) | (1u << 4) | 0x3u | (3u << 2));
}

bool psram_present(void)
{
    return s_psram_present;
}

bool psram_init(void)
{
    /* SPI0 and SPI1 share the core clock selection. */
    wr(SPI_CORE_CLK_SEL(0), 2);                /* 160 MHz */
    set_spi_clock(0, FLASH_DIV);
    set_spi_clock(1, BRINGUP_DIV);

    init_psram_pins();

    /* CS setup/hold for the Ext_RAM path. The chip needs a minimum CS high time
     * between transactions; the reset defaults are too short for octal mode. */
    wr(SPI_SMEM_AC(0), (rd(SPI_SMEM_AC(0)) &
                        ~((AC_CS_HOLD_TIME_V << AC_CS_HOLD_TIME_S) |
                          (AC_CS_SETUP_TIME_V << AC_CS_SETUP_TIME_S) |
                          (AC_CS_HOLD_DELAY_V << AC_CS_HOLD_DELAY_S))) |
                       AC_CS_SETUP | AC_CS_HOLD |
                       (3u << AC_CS_HOLD_TIME_S) |
                       (3u << AC_CS_SETUP_TIME_S) |
                       (2u << AC_CS_HOLD_DELAY_S));

    /* ECC is off. The chip's own ECC would need the 16-to-17 byte page mode, and
     * the address the MMU hands out would no longer be a plain byte offset. */
    wr(SYSCON_SRAM_ACE0, rd(SYSCON_SRAM_ACE0) & ~SRAM_ECC_ENABLE);

    /* Variable dummy length on SPI1's read path; the fixed default is not right
     * for the PSRAM's latency. */
    wr(SPI_FMEM_DDR(1), rd(SPI_FMEM_DDR(1)) | DDR_VAR_DUMMY);
    /* No DTR byte swap: that is the reset state, and ESP-IDF's ROM call with
     * (false, false) amounts to the same thing. */
    wr(SPI_FMEM_DDR(1), rd(SPI_FMEM_DDR(1)) & ~(DDR_WDAT_SWP | DDR_RDAT_SWP));

    /* MR0: fixed latency (lt = 1), read latency 2, drive strength 0. Only the
     * first 8 bits of the data phase are the mode register. */
    if (!opi_command(OPI_REG_WRITE, CMD_BITS, 0x0, ADDR_BITS, 0,
                     (1u << 5) | (2u << 2), 16, NULL, 0))
        return false;

    /* Prove the chip is there before mapping anything. */
    uint32_t ref = 0x5a6b7c8d, got = 0;

    if (!opi_command(OPI_SYNC_WRITE, CMD_BITS, 0x0, ADDR_BITS, WR_DUMMY_BITS,
                     ref, 32, NULL, 0))
        return false;

    if (!opi_command(OPI_SYNC_READ, CMD_BITS, 0x0, ADDR_BITS, RD_DUMMY_BITS,
                     0, 0, &got, 32))
        return false;

    if (got != ref)
        return false;

    /* Now SPI0 can reach the chip. Command, address and dummy phases all octal,
     * 32-bit address, and the chip's own read/write command words. */
    wr(SPI_CACHE_SCTRL(0), rd(SPI_CACHE_SCTRL(0)) | SCTRL_USR_WCMD | SCTRL_USR_RCMD |
                           SCTRL_SCMD_4BYTE | SCTRL_USR_RD_DUMMY | SCTRL_USR_WR_DUMMY |
                           SCTRL_SRAM_OCT);

    wr(SPI_SRAM_DWR(0), (15u << 28) | OPI_SYNC_WRITE);
    wr(SPI_SRAM_DRD(0), (15u << 28) | OPI_SYNC_READ);
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 14)) | (31u << 14));
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 6)) |
                           ((RD_DUMMY_BITS - 1) << 6));
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 22)) |
                           ((WR_DUMMY_BITS - 1) << 22));

    wr(SPI_SMEM_DDR(0), (rd(SPI_SMEM_DDR(0)) & ~(DDR_RDAT_SWP | DDR_WDAT_SWP)) | DDR_EN);
    wr(SPI_SRAM_CMD(0), rd(SPI_SRAM_CMD(0)) | SCMD_SDUMMY_OUT | SCMD_SCMD_OCT |
                        SCMD_SADDR_OCT | SCMD_SDOUT_OCT | SCMD_SDIN_OCT);

    set_psram_clock(PSRAM_DIV);

    s_psram_present = true;
    return true;
}

#endif /* CPU_XTENSA */
