/***************************************************************************
 * ESP32-S3 octal PSRAM bring-up.
 *
 * The ESP32-S3 has no external RAM controller in the sense the rest of Rockbox
 * expects. The 32 MB window at 0x3C000000 is the MSPI (SPI0) "external memory"
 * view, and a 512-entry MMU decides per 64 KB page whether that page is flash or
 * PSRAM. The chip itself has to be told to speak octal before any of that works.
 *
 * Neither the mask ROM nor ESP-IDF's second-stage bootloader does this: ESP-IDF
 * does it in the application, in components/esp_psram/esp32s3/psram_oct.c.
 * Rockbox is the application, so the bring-up has to happen here, before
 * mmu_init() maps the pages the audiobuffer, framebuffer and codec buffers live
 * in.
 *
 * The device is the 8 MB octal PSRAM on the WROOM-1-N16R8 module. It hangs off
 * SPI1 - the second instance of the same hardware block, 0x1000 below SPI0 - on
 * CS1, and is reached through SPI0's Ext_RAM path once that path's command,
 * address and dummy phases are configured for octal mode.
 *
 * The transaction itself is issued through the ROM's esp_rom_opiflash_exec_cmd(),
 * which is exactly what ESP-IDF uses. Under the windowed ABI it is callable
 * straight from C: the first four arguments are in a2-a5 and the remaining nine
 * go on the stack. That removes the hand-built SPI sequencing that used to live
 * here, and with it the class of bug where one phase of a thirteen-phase
 * transaction is set up slightly differently from what the hardware expects.
 *
 * Register offsets and bit positions are from
 * components/soc/esp32s3/register/soc/spi_mem_reg.h.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "xtensa.h"
#include "gpio-xtensa.h"
#include "psram-xtensa.h"
#include "bootlog.h"

#ifdef CPU_XTENSA

/* SPI_MEM instances: 0 is SPI0 (the cache/MMU path), 1 is SPI1 (user commands). */
#define SPI_MEM_BASE(i)       (0x60003000u - (unsigned)(i) * 0x1000u)

#define MMU_TABLE_BASE        0x600C5000u
#define MMU_PAGE              0x10000u
#define MMU_INVALID           (1u << 14)
#define MMU_TARGET_PSRAM      (1u << 15)
#define FLASH_WINDOW_LOW      0x3C000000u

#define SPI_CMD(i)            (SPI_MEM_BASE(i) + 0x00)
#define SPI_ADDR(i)           (SPI_MEM_BASE(i) + 0x04)
#define SPI_CTRL(i)           (SPI_MEM_BASE(i) + 0x08)
#define SPI_CLOCK(i)          (SPI_MEM_BASE(i) + 0x14)
#define SPI_MISC(i)           (SPI_MEM_BASE(i) + 0x34)
#define SPI_CACHE_SCTRL(i)    (SPI_MEM_BASE(i) + 0x40)
#define SPI_CACHE_FCTRL(i)    (SPI_MEM_BASE(i) + 0x3C)
#define SPI_SRAM_CMD(i)       (SPI_MEM_BASE(i) + 0x44)
#define SPI_SRAM_DRD(i)       (SPI_MEM_BASE(i) + 0x48)
#define SPI_SRAM_DWR(i)       (SPI_MEM_BASE(i) + 0x4C)
#define SPI_SRAM_CLK(i)       (SPI_MEM_BASE(i) + 0x50)
#define SPI_FSM(i)            (SPI_MEM_BASE(i) + 0x54)
#define SPI_CORE_CLK_SEL(i)   (SPI_MEM_BASE(i) + 0xEC)
#define SPI_SMEM_AC(i)        (SPI_MEM_BASE(i) + 0xDC)
#define SPI_SMEM_CALI(i)      (SPI_MEM_BASE(i) + 0xBC)
#define SPI_SMEM_DIN_MODE(i)  (SPI_MEM_BASE(i) + 0xC0)
#define SPI_SMEM_DIN_NUM(i)   (SPI_MEM_BASE(i) + 0xC4)
#define SPI_FMEM_DDR(i)       (SPI_MEM_BASE(i) + 0xE0)
#define SPI_SMEM_DDR(i)       (SPI_MEM_BASE(i) + 0xE4)
#define SPI_CLK_GATE(i)       (SPI_MEM_BASE(i) + 0xE8)
#define SPI_TIMING_CALI(i)    (SPI_MEM_BASE(i) + 0xA8)   /* flash-side tuning  */
#define SPI_DIN_MODE(i)       (SPI_MEM_BASE(i) + 0xAC)
#define SPI_DIN_NUM(i)        (SPI_MEM_BASE(i) + 0xB0)
#define SPI_RD_STATUS(i)      (SPI_MEM_BASE(i) + 0x2C)
#define SPI_INT_RAW(i)        (SPI_MEM_BASE(i) + 0xF8)
#define SPI_USER(i)           (SPI_MEM_BASE(i) + 0x18)
#define SPI_USER1(i)          (SPI_MEM_BASE(i) + 0x1C)
#define SPI_USER2(i)          (SPI_MEM_BASE(i) + 0x20)
#define SPI_MOSI_DLEN(i)      (SPI_MEM_BASE(i) + 0x24)
#define SPI_MISO_DLEN(i)      (SPI_MEM_BASE(i) + 0x28)
#define SPI_W0(i)             (SPI_MEM_BASE(i) + 0x58)
#define SPI_MEM_INT_RAW(i)    (SPI_MEM_BASE(i) + 0xF8)
#define SPI_DATE(i)           (SPI_MEM_BASE(i) + 0x3FC)

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
#define RD_DUMMY_BITS       2 * (10 - 1)     /* 18 */
#define WR_DUMMY_BITS       2 * (5 - 1)      /* 8 */
#define MR_DUMMY_BITS       8

#define IO_MUX_SPICS1       (DR_REG_IO_MUX_BASE + 0x6C)
#define IO_MUX_FUN_DRV_S    10

#define SYSCON_BASE         0x60026000
#define SYSCON_SRAM_ACE0    (SYSCON_BASE + 0x58)
#define EXTMEM_DCACHE_CTRL  0x600C4000u
#define EXTMEM_ICACHE_CTRL  0x600C4060u
#define EXTMEM_CACHE_STATE  0x600C4130u
#define EXTMEM_DCACHE_PRELOAD_CTRL 0x600C4040u
#define EXTMEM_DBUS_REJECT_ST      0x600C4100u
#define EXTMEM_DBUS_REJECT_VADDR   0x600C4104u
#define EXTMEM_CACHE_MMU_FAULT     0x600C4120u
#define SRAM_ECC_ENABLE     (1u << 8)

/* ROM entry points, from components/esp_rom/esp32s3/ld/esp32s3.rom.ld. Each is a
 * two-instruction trampoline (l32r + jx) into the real ROM routine, so the window
 * is set by the call itself, not by the trampoline. */
#define ROM_SPI_SET_OP_MODE     0x400008a0u
#define ROM_OPI_EXEC_CMD        0x400008b8u
#define ROM_SPI_SET_DTR_SWAP    0x4000093cu
#define ROM_CACHE_RESUME_DCACHE 0x400018c0u
#define ROM_CACHE_INVALIDATE_D  0x40001680u
#define ROM_CACHE_DISABLE_ICACHE 0x4000186cu
#define ROM_CACHE_ENABLE_ICACHE  0x40001878u
#define ROM_CACHE_DISABLE_DCACHE 0x40001884u
#define ROM_CACHE_ENABLE_DCACHE  0x40001890u
#define ROM_CACHE_START_PRELOAD 0x40001770u
#define ROM_CACHE_PRELOAD_DONE  0x4000177cu
#define ROM_CACHE_END_PRELOAD   0x40001788u

/* esp_rom_spiflash_read_mode_t, from components/esp_rom/include/esp32s3/rom/spi_flash.h:
 * QIO=0, QOUT=1, DIO=2, DOUT=3, FASTRD=4, SLOWRD=5, OPI_STR=6, OPI_DTR=7.
 * The PSRAM chip only answers in DTR; passing 4 selects DOUT and the whole
 * transaction runs single-line, which is why the chip looked dead.
 */
#define OPI_DTR_MODE            7

/* MSPI clock dividers. The core clock is selected in SPI_MEM_CORE_CLK_SEL_REG and
 * the module clocks are derived from it: 80 MHz core / 4 = 20 MHz during the chip
 * bring-up, 160 MHz core / 2 = 80 MHz afterwards. Both MSPI instances share the
 * core-clock register, and the flash read FSM is shared too, which is why ESP-IDF
 * always slows SPI0 and SPI1 together.
 */
#define CORE_CLK_80         0
#define CORE_CLK_160        2
#define LOW_SPEED_DIV       4      /* 20 MHz from the 80 MHz core clock */
#define HIGH_SPEED_DIV      2      /* 80 MHz from the 160 MHz core clock */

/* ESP-IDF's PSRAM timing table for a 160 MHz core clock and an 80 MHz module clock
 * in DTR mode, from
 * components/esp_mspi/mspi_timing_tuning/port/esp32s3/mspi_timing_tuning_configs.h.
 * Each row is (spi_din_mode, spi_din_num, extra_dummy_len); the sweep tries all 14
 * and keeps the longest run that reads the reference pattern back correctly.
 * default_config_id for this table is 5, used when the sweep finds no run of 3-5.
 */
typedef struct { uint8_t din_mode, din_num, extra_dummy; } mspi_tune_t;

static const mspi_tune_t psram_tune_table[14] = {
    {0, 0, 0}, {4, 2, 2}, {2, 1, 2}, {4, 1, 2}, {1, 0, 1}, {4, 0, 2}, {0, 0, 1},
    {4, 2, 3}, {2, 1, 3}, {4, 1, 3}, {1, 0, 2}, {4, 0, 3}, {0, 0, 2}, {4, 2, 4}
};
#define PSRAM_DEFAULT_TUNE  5
#define PSRAM_TEST_WORDS    16     /* MSPI_TIMING_TEST_DATA_LEN is 64 bytes */

/* Chicken and egg: this whole path reconfigures the MSPI block, and the MSPI block
 * is what feeds the flash instruction-cache window that .text is executed from.
 * So every function here is ICODE_ATTR: it runs from .iram, which the mask ROM
 * loader copies into internal SRAM before it jumps to the entry point. Internal
 * SRAM needs no MMU page, no cache and no flash read to be fetched.
 *
 * The same applies to boot_trace(): a marker printed from .text is a fetch from
 * the window being reconfigured.
 */
static bool s_psram_present __attribute__((section(".srambss"))) = false;

static inline ICODE_ATTR uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline ICODE_ATTR void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)reg = v; }

static void ICODE_ATTR set_spi_clock(int spi, unsigned div)
{
    if (div <= 1)
        wr(SPI_CLOCK(spi), 1u << 31);          /* CLK_EQU_SYSCLK */
    else
        wr(SPI_CLOCK(spi), ((div - 1) << 16) | ((div / 2 - 1) << 8) | (div - 1));
}

static void ICODE_ATTR set_psram_clock(unsigned div)
{
    if (div <= 1)
        wr(SPI_SRAM_CLK(0), 1u << 31);         /* SCLK_EQU_SYSCLK */
    else
        wr(SPI_SRAM_CLK(0), ((div - 1) << 16) | ((div / 2 - 1) << 8) | (div - 1));
}

/* Write the nine DIN0..DIN7 + DQS fields of a DIN register with one value. The
 * mode fields are 3 bits at offsets 0,3,...,24; the num fields are 2 bits at
 * 0,2,...,18. Same layout on the flash side and the PSRAM side.
 */
static void ICODE_ATTR set_din_mode_num(int spi, unsigned mode, unsigned num)
{
    uint32_t m = 0, n = 0;
    int i;

    for (i = 0; i < 9; i++) {
        m |= (mode & 7u) << (3 * i);
        n |= (num  & 3u) << (2 * i);
    }
    wr(SPI_DIN_MODE(spi), m);
    wr(SPI_DIN_NUM(spi), n);
}

static void ICODE_ATTR set_psram_din_mode_num(int spi, unsigned mode, unsigned num)
{
    uint32_t m = 0, n = 0;
    int i;

    for (i = 0; i < 9; i++) {
        m |= (mode & 7u) << (3 * i);
        n |= (num  & 3u) << (2 * i);
    }
    wr(SPI_SMEM_DIN_MODE(spi), m);
    wr(SPI_SMEM_DIN_NUM(spi), n);
}

/* The extra-dummy register is a calibration enable (bit 1) plus a length in the
 * bits above it. Clearing the enable returns the transaction to its un-tuned form.
 */
static void ICODE_ATTR set_extra_dummy(int reg, unsigned len)
{
    if (len)
        wr(reg, (rd(reg) & ~(1u << 1)) | (1u << 1) | ((len & 7u) << 2));
    else
        wr(reg, rd(reg) & ~((1u << 1) | (7u << 2)));
}

/* ESP-IDF's mspi_timing_enter_low_speed_mode(): 80 MHz core, 20 MHz on both MSPI
 * instances, and every tuning register back to zero. The DIN registers are shared,
 * so clearing them for SPI1 while SPI0 still runs fast would break the flash path;
 * that is why both instances are slowed together.
 */
static void ICODE_ATTR mspi_enter_low_speed(void)
{
    wr(SPI_CORE_CLK_SEL(0), CORE_CLK_80);
    wr(SPI_CORE_CLK_SEL(1), CORE_CLK_80);
    set_spi_clock(0, LOW_SPEED_DIV);
    set_spi_clock(1, LOW_SPEED_DIV);
    set_psram_clock(LOW_SPEED_DIV);
    set_din_mode_num(0, 0, 0);
    set_psram_din_mode_num(0, 0, 0);
    set_extra_dummy(SPI_TIMING_CALI(0), 0);
    set_extra_dummy(SPI_TIMING_CALI(1), 0);
    set_extra_dummy(SPI_SMEM_CALI(0), 0);
    set_extra_dummy(SPI_SMEM_CALI(1), 0);
}

static void ICODE_ATTR init_psram_pins(void)
{
    /* CS1, HD, WP, CLK, Q0-Q3, D0-D3 all live on the IO MUX, so only the pad
     * function and drive strength need touching. ESP-IDF uses the strongest
     * driver on every one of them; pads 29 (CS0) and 30 (SPICLK) are in the list
     * too, though the clock pad's drive is overridden by SPI_MEM_DATE_REG below.
     */
    wr(IO_MUX_SPICS1, (rd(IO_MUX_SPICS1) & ~((7u << 12) | (3u << 10))) | (3u << 10));

    /* Octal needs eight data lines. D0-D3 are SPID(32), SPIQ(31), SPIHD(27) and
     * SPIWP(28); D4-D7 and DQS are the SPIIO4..7 / SPIDQS functions on pads
     * 33..37. The ROM routes those only when the efuses describe an OPI *flash*,
     * which is not the case on a quad-flash module with octal PSRAM, so they come
     * back in GPIO mode and the chip sees four of its eight lines.
     */
    for (int pin = 26; pin <= 32; pin++)
        wr(IO_MUX_PIN(pin), (rd(IO_MUX_PIN(pin)) & ~((7u << 12) | (3u << 10))) |
                             (3u << 10));
    for (int pin = 33; pin <= 37; pin++)
        wr(IO_MUX_PIN(pin), (rd(IO_MUX_PIN(pin)) & ~((7u << 12) | (3u << 10))) |
                             (4u << 12) | (3u << 10));

    /* The SPI clock pad's drive strength is controlled from the SPI block
     * itself, not the IO MUX: enable the override, then set both the Ext_RAM and
     * flash-side clock pads to the strongest driver. */
    wr(SPI_DATE(0), rd(SPI_DATE(0)) | (1u << 4) | 0x3u | (3u << 2));
}

static void ICODE_ATTR set_psram_cs_timing(void)
{
    /* SPI0 and SPI1 share the CS setup/hold and CS hold delay registers for
     * PSRAM, so only SPI0 needs writing. Values are ESP-IDF's OCT_PSRAM_CS_*
     * constants from components/esp_psram/esp32s3/esp_psram_impl_octal.c.
     */
    wr(SPI_SMEM_AC(0), (rd(SPI_SMEM_AC(0)) &
                        ~((AC_CS_HOLD_TIME_V << AC_CS_HOLD_TIME_S) |
                          (AC_CS_SETUP_TIME_V << AC_CS_SETUP_TIME_S) |
                          (AC_CS_HOLD_DELAY_V << AC_CS_HOLD_DELAY_S))) |
                       AC_CS_SETUP | AC_CS_HOLD |
                       (3u << AC_CS_HOLD_TIME_S) |
                       (3u << AC_CS_SETUP_TIME_S) |
                       (2u << AC_CS_HOLD_DELAY_S) |
                       (3u << 12));  /* ECC_CS_HOLD_TIME = 3 */

    /* ESP-IDF sets ECC_SKIP_PAGE_CORNER even with ECC off. */
    wr(SPI_SMEM_AC(0), rd(SPI_SMEM_AC(0)) | (1u << 15));
}

/* What the MSPI instance actually holds after a user command. The chip either
 * answers or it does not, and the register file distinguishes "the transaction ran
 * and the data lines floated" from "the transaction never started".
 */
/*
 * One octal transaction on an MSPI instance, programmed by hand.
 *
 * The ROM wrapper is not used here. Measured on this board, a call through
 * esp_rom_opiflash_exec_cmd() programs ADDR and MISO_DLEN but leaves USER1 and
 * USER2 at their reset values - USR_COMMAND_BITLEN stays 7 (8 bits) and
 * USR_ADDR_BITLEN stays 23 - so the transaction that reaches the chip is not the
 * one we asked for. Writing the phases explicitly removes that class of bug.
 *
 * Phase lengths are all stored as (bit_num - 1). In octal mode the clock moves
 * eight bits per cycle, so the dummy phase is expressed in cycles.
 */
/* The ROM's own OPI transaction, exactly the call ESP-IDF makes. Under the
 * windowed ABI the first four arguments are in a2-a5 and the remaining nine sit on
 * the stack, so a plain function-pointer call marshals them correctly.
 *
 * esp_rom_opiflash_exec_cmd(int spi_num, esp_rom_spiflash_read_mode_t mode,
 *                            uint32_t cmd, int cmd_bit_len, uint32_t addr,
 *                            int addr_bit_len, int dummy_bits, uint8_t *mosi_data,
 *                            int mosi_bit_len, uint8_t *miso_data, int miso_bit_len,
 *                            uint16_t cs_mask, bool is_write_erase_cmd)
 */
typedef void (*opi_exec_t)(int, int, uint32_t, int, uint32_t, int, int,
                           const uint32_t *, int, uint32_t *, int, int, int);

static uint32_t ICODE_ATTR rom_read_mr(int spi, uint32_t reg)
{
    uint32_t v = 0;

    ((opi_exec_t)ROM_OPI_EXEC_CMD)(spi, OPI_DTR_MODE, OPI_REG_READ, CMD_BITS,
                                   reg, ADDR_BITS, RD_DUMMY_BITS,
                                   NULL, 0, &v, 16, 1 << 1, 0);
    return v;
}

static void ICODE_ATTR rom_write_mr(int spi, uint32_t reg, uint32_t val)
{
    ((opi_exec_t)ROM_OPI_EXEC_CMD)(spi, OPI_DTR_MODE, OPI_REG_WRITE, CMD_BITS,
                                   reg, ADDR_BITS, 0,
                                   &val, 16, NULL, 0, 1 << 1, 0);
}

/* One 32-bit word into the PSRAM's own address space, through SPI1. */
static void ICODE_ATTR rom_write_word(int spi, uint32_t addr, uint32_t val)
{
    ((opi_exec_t)ROM_OPI_EXEC_CMD)(spi, OPI_DTR_MODE, OPI_SYNC_WRITE, CMD_BITS,
                                   addr, ADDR_BITS, WR_DUMMY_BITS,
                                   &val, 32, NULL, 0, 1 << 1, 0);
}

static uint32_t ICODE_ATTR rom_read_word(int spi, uint32_t addr)
{
    uint32_t v = 0;

    ((opi_exec_t)ROM_OPI_EXEC_CMD)(spi, OPI_DTR_MODE, OPI_SYNC_READ, CMD_BITS,
                                   addr, ADDR_BITS, RD_DUMMY_BITS,
                                   NULL, 0, &v, 32, 1 << 1, 0);
    return v;
}

/* Identify the chip from its mode registers.
 *
 * A mode-register read is self-authenticating in a way a sync read is not. We write
 * 0x28 to MR0 and get back 0x0d28: the low byte is exactly what we sent, and the
 * high byte is MR1 - the vendor id - which we never sent. A FIFO echoing the last
 * transmitted word could not produce it.
 *
 * MR1 vendor 0x0D is Apotek, the part OCT_PSRAM_VENDOR_ID names; MR2 bits 2:0 give
 * the density (3 = 64 Mbit = 8 MB on the WROOM-1-N16R8); MR3 bit 6 is the VCC
 * selection (1 = 3V, i.e. the AP_3v3 variant).
 */
static bool ICODE_ATTR psram_identify(int spi)
{
    uint32_t mr01 = rom_read_mr(spi, 0x0);
    uint32_t mr23 = rom_read_mr(spi, 0x2);
    uint32_t mr8  = rom_read_mr(spi, 0x8);
    uint32_t vendor = (mr01 >> 8) & 0x1fu;
    int density = mr23 & 7;

    boot_trace('i');
    boot_trace_hex(mr01);
    boot_trace_hex(mr23);
    boot_trace_hex(mr8);

    return vendor == 0x0Du && density != 0;
}

/* The decisive test: does SPI0's Ext_RAM D-cache serve a load from a page the MMU
 * maps to PSRAM? Everything Rockbox needs - audiobuffer, framebuffer, codecbuf -
 * is reached that way, so a working SPI1 user command is not enough on its own.
 *
 * The preload is used as a probe rather than a direct load because a direct load
 * on a stalled FSM hangs the CPU with no way to observe it. Cache_Start_DCache_Preload
 * drives the same FSM asynchronously, so its DONE flag can be polled with a guard.
 */
/* The external caches sit on the same MSPI block whose clocks the bring-up is
 * changing. ESP-IDF suspends them before every speed switch (spi_flash_disable_cache
 * / spi_flash_restore_cache around mspi_timing_enter_*_early) so no fetch is in
 * flight while the divider moves. Everything in this file is ICODE_ATTR, so it runs
 * from internal SRAM and does not need the flash window while the cache is down.
 */
static void ICODE_ATTR extmem_cache_suspend(void)
{
    wr(EXTMEM_ICACHE_CTRL, rd(EXTMEM_ICACHE_CTRL) & ~1u);
    wr(EXTMEM_DCACHE_CTRL, rd(EXTMEM_DCACHE_CTRL) & ~1u);
}

static void ICODE_ATTR extmem_cache_resume(void)
{
    wr(EXTMEM_ICACHE_CTRL, rd(EXTMEM_ICACHE_CTRL) | 1u);
    wr(EXTMEM_DCACHE_CTRL, rd(EXTMEM_DCACHE_CTRL) | 1u);
}

/* ESP-IDF writes 64 bytes of 0xa5ff005a at PSRAM address 0 once, before the sweep,
 * and each candidate configuration only reads them back. Address 0 here is the chip's
 * own address space (MSPI_TIMING_PSRAM_TEST_DATA_ADDR), not the MMU window, so the
 * test needs no mapping.
 */
static void ICODE_ATTR psram_write_reference(int spi)
{
    for (int i = 0; i < PSRAM_TEST_WORDS; i++)
        rom_write_word(spi, (uint32_t)i * 4u, 0xa5ff005au);
}

static bool ICODE_ATTR psram_read_reference(int spi)
{
    for (int i = 0; i < PSRAM_TEST_WORDS; i++)
        if (rom_read_word(spi, (uint32_t)i * 4u) != 0xa5ff005au)
            return false;
    return true;
}

/* mspi_timing_psram_tuning(), reduced to the DIN sweep that SOC_MEMSPI_TIMING_TUNING_BY_MSPI_DELAY
 * uses on this chip. The reference data is written at 20 MHz; the sweep then runs at
 * the final 80 MHz module clock, which is the speed the Ext_RAM cache will actually
 * use.
 */
static int ICODE_ATTR psram_tune(void)
{
    int run = 0, best = 0, end = 0;

    wr(SPI_CORE_CLK_SEL(0), CORE_CLK_160);
    wr(SPI_CORE_CLK_SEL(1), CORE_CLK_160);
    set_spi_clock(0, HIGH_SPEED_DIV);
    set_spi_clock(1, HIGH_SPEED_DIV);
    /* SPI_MEM_SPI_SMEM_TIMING_CALI_REG bit 0 is the PSRAM timing-adjust clock enable. */
    wr(SPI_SMEM_CALI(0), rd(SPI_SMEM_CALI(0)) | 1u);
    /* Variable dummy is disabled during the sweep so the dummy length is exactly what
     * the DIN registers add. */
    wr(SPI_FMEM_DDR(1), rd(SPI_FMEM_DDR(1)) & ~DDR_VAR_DUMMY);

    for (int i = 0; i < 14; i++) {
        set_psram_din_mode_num(0, psram_tune_table[i].din_mode,
                               psram_tune_table[i].din_num);
        set_extra_dummy(SPI_TIMING_CALI(1), psram_tune_table[i].extra_dummy);

        if (psram_read_reference(1)) {
            run++;
            if (run > best) {
                best = run;
                end = i;
            }
        } else {
            run = 0;
        }
    }

    boot_trace('Y');
    boot_trace_hex(best);
    boot_trace_hex(end);

    /* s_select_best_tuning_config_dtr, 160 MHz core clock: a run of 3-4 takes the
     * second-to-last point of the run, a run of 5 the third-to-last. Anything else is
     * a failed sweep and falls back to the table's default_config_id.
     */
    if (best <= 2 || best >= 6)
        return PSRAM_DEFAULT_TUNE;
    if (best <= 4)
        return end - 1;
    return end - 2;
}

/* mspi_timing_enter_high_speed_mode(): both instances back to 80 MHz module clock,
 * the Ext_RAM PSRAM clock at 80 MHz, and the winning DIN/dummy values applied to
 * SPI0, which owns the cache path.
 */
static void ICODE_ATTR mspi_enter_high_speed(int tune_id)
{
    const mspi_tune_t *c = &psram_tune_table[tune_id];

    wr(SPI_CORE_CLK_SEL(0), CORE_CLK_160);
    wr(SPI_CORE_CLK_SEL(1), CORE_CLK_160);
    set_spi_clock(0, HIGH_SPEED_DIV);
    set_spi_clock(1, HIGH_SPEED_DIV);
    set_psram_clock(HIGH_SPEED_DIV);
    set_psram_din_mode_num(0, c->din_mode, c->din_num);
    set_extra_dummy(SPI_SMEM_CALI(0), c->extra_dummy);
}

static bool ICODE_ATTR cache_psram_probe(void)
{
    /* Entry 100 is inside the 8 MB PSRAM (128 pages) and above the pages the ROM
     * loader mapped: the DROM segment is 0x3c000020 + 0x7a194, i.e. entries 0..7.
     */
    const int entry = 100;
    const uint32_t vaddr = FLASH_WINDOW_LOW + (uint32_t)entry * MMU_PAGE;
    uint32_t guard = 0, bad = 0, i;

    *(volatile uint32_t *)(MMU_TABLE_BASE + 4u * (unsigned)entry) =
        MMU_TARGET_PSRAM | (uint32_t)entry;
    boot_trace('C');
    boot_trace_hex(rd(MMU_TABLE_BASE + 4u * (unsigned)entry));
    boot_trace_hex(vaddr);

    ((void (*)(uint32_t, uint32_t))ROM_CACHE_INVALIDATE_D)(vaddr, MMU_PAGE);

    /* A bulk write, not one word. Single-word probes pass on this board even when
     * the Ext_RAM read path is mistuned, so the test has to look like the thing
     * Rockbox actually does: several kilobytes written and read back.
     */
    for (i = 0; i < 4096; i++)
        *(volatile uint32_t *)(vaddr + i * 4u) = 0xa5ff005au + i;
    boot_trace('B');
    for (i = 0; i < 4096; i++)
        if (*(volatile uint32_t *)(vaddr + i * 4u) != 0xa5ff005au + i)
            bad++;
    boot_trace_hex(bad);

    boot_trace_hex(rd(EXTMEM_CACHE_STATE));
    boot_trace_hex(rd(EXTMEM_DCACHE_PRELOAD_CTRL));
    ((void (*)(uint32_t, uint32_t))ROM_CACHE_START_PRELOAD)(vaddr, MMU_PAGE);
    while (!((int (*)(void))ROM_CACHE_PRELOAD_DONE)() && guard++ < 2000000u)
        ;
    boot_trace_hex(rd(EXTMEM_CACHE_STATE));
    boot_trace_hex(rd(EXTMEM_DBUS_REJECT_ST));
    boot_trace_hex(rd(EXTMEM_DBUS_REJECT_VADDR));
    boot_trace_hex(rd(EXTMEM_CACHE_MMU_FAULT));
    boot_trace_hex(rd(SPI_FSM(0)));
    boot_trace_hex(guard);
    ((void (*)(uint32_t, uint32_t))ROM_CACHE_END_PRELOAD)(vaddr, MMU_PAGE);

    if (guard >= 2000000u)
        return false;

    return bad == 0;
}

/* SPI0's Ext_RAM cache path: the hardware builds the octal transaction itself from
 * CACHE_SCTRL, SRAM_CMD, SRAM_DRD and SRAM_DWR, so a plain load through a
 * PSRAM-mapped MMU page needs no software at all once these are set. */
static void ICODE_ATTR config_psram_spi_phases(void)
{
    wr(SPI_CACHE_SCTRL(0), rd(SPI_CACHE_SCTRL(0)) | SCTRL_USR_WCMD | SCTRL_USR_RCMD);
    wr(SPI_SRAM_DWR(0), (15u << 28) | OPI_SYNC_WRITE);
    wr(SPI_SRAM_DRD(0), (15u << 28) | OPI_SYNC_READ);

    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 14)) | (31u << 14));
    wr(SPI_CACHE_SCTRL(0), rd(SPI_CACHE_SCTRL(0)) | SCTRL_SCMD_4BYTE);

    wr(SPI_CACHE_SCTRL(0), rd(SPI_CACHE_SCTRL(0)) | SCTRL_USR_RD_DUMMY |
                           SCTRL_USR_WR_DUMMY);
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 6)) |
                           ((RD_DUMMY_BITS - 1u) << 6));
    wr(SPI_SMEM_DDR(0), (rd(SPI_SMEM_DDR(0)) & ~(DDR_RDAT_SWP | DDR_WDAT_SWP)) |
                        DDR_EN | DDR_VAR_DUMMY);
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 22)) |
                           ((WR_DUMMY_BITS - 1u) << 22));

    wr(SPI_SRAM_CMD(0), rd(SPI_SRAM_CMD(0)) | SCMD_SDUMMY_OUT | SCMD_SCMD_OCT |
                        SCMD_SADDR_OCT | SCMD_SDOUT_OCT | SCMD_SDIN_OCT);
    wr(SPI_CACHE_SCTRL(0), rd(SPI_CACHE_SCTRL(0)) | SCTRL_SRAM_OCT);
}

bool ICODE_ATTR psram_present(void)
{
    return s_psram_present;
}

/* Does SPI1's user-command path reach the physical MSPI pins at all? The flash is
 * on the same block and the same pins, only on CS0. A plain serial read of a known
 * flash offset through SPI1 separates "SPI1 user commands work" from "the CS1 side
 * is unreachable".
 */
bool ICODE_ATTR psram_init(void)
{
    int tune;

    boot_trace('p');

    /* ESP-IDF wraps every MSPI speed change in a cache suspend/restore
     * (spi_flash_disable_cache / spi_flash_restore_cache around
     * mspi_timing_enter_low_speed_early). The block whose divider is about to move
     * is the same block the instruction and data caches read flash through, so no
     * fetch should be in flight while it changes. Everything here is ICODE_ATTR and
     * runs from internal SRAM, so the boot survives the cache being down.
     */
    extmem_cache_suspend();
    mspi_enter_low_speed();
    extmem_cache_resume();

    wr(SPI_CLK_GATE(0), rd(SPI_CLK_GATE(0)) | 1u);
    wr(SPI_CLK_GATE(1), rd(SPI_CLK_GATE(1)) | 1u);
    /* The CMD/ADDR/DOUT/DIN/DUMMY phases of a user command run in octal only if
     * SPI1's CTRL says so. The ROM leaves these set after a quad-flash boot on this
     * module, but the bring-up must not depend on that. */
    wr(SPI_CTRL(1), rd(SPI_CTRL(1)) |
                    (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 9));

    /* ESP-IDF's s_init_psram_pins() only sets pad 26 to SPICS1 and the clock drive
     * strength; it never routes pads 33..37 to SPIIO4..7/SPIDQS. Measured on this
     * board, the mask ROM leaves pads 33..37 at MCU_SEL=0 (GPIO) and pads 29..32 at
     * MCU_SEL=0 (dedicated SPI), so Rockbox has to route 33..37 itself - which
     * init_psram_pins() does.
     */
    init_psram_pins();
    set_psram_cs_timing();

    /* ECC is off. The chip's own ECC would need the 16-to-17 byte page mode, and
     * the address the MMU hands out would no longer be a plain byte offset. */
    wr(SYSCON_SRAM_ACE0, rd(SYSCON_SRAM_ACE0) & ~SRAM_ECC_ENABLE);

    /* Octal PSRAM is DTR-only (quad PSRAM is STR), so the double-data-rate enable
     * has to be on for the instance that talks to the chip. ESP-IDF only sets the
     * variable-dummy bit here; Rockbox also makes sure DDR_EN is set, and clears the
     * DTR swap bits as esp_rom_spi_set_dtr_swap_mode(1, false, false) does.
     */
    wr(SPI_FMEM_DDR(1), (rd(SPI_FMEM_DDR(1)) & ~0xCu) | DDR_EN | DDR_VAR_DUMMY);
    { typedef void (*dtr_t)(int, int, int);
      ((dtr_t)ROM_SPI_SET_DTR_SWAP)(1, 0, 0); }

    boot_trace('c');

    /* ESP-IDF drives the PSRAM through SPI1. SPI0 cannot be used for this: its
     * user-command FSM is the same hardware the instruction and data caches use to
     * read flash, and issuing a user command on it leaves the flash read path
     * stalled, so every later fetch through the 0x42000000 window returns the
     * 0x0bad0bad "no mapping" sentinel and the boot dies on an IllegalInstruction.
     */
    boot_trace('1');

    /* MR0: lt = 1 (fixed latency), read_latency = 2, drive_str = 0.
     * MR8: bl = 3 (1024-byte burst), bt = 0. */
    rom_write_mr(1, 0x0, (1u << 5) | (2u << 2));
    rom_write_mr(1, 0x8, 3u);

    if (!psram_identify(1)) {
        /* The chip never answered. Put the MSPI clocks back so the flash path still
         * works, and report the absence rather than leaving the boot stalled. */
        extmem_cache_suspend();
        mspi_enter_high_speed(PSRAM_DEFAULT_TUNE);
        extmem_cache_resume();
        s_psram_present = false;
        boot_trace('!');
        return false;
    }

    boot_trace('4');

    /* The reference pattern is written once, at the 20 MHz bring-up clock, and the
     * sweep then runs at the 80 MHz the Ext_RAM cache will use. This is the step
     * the previous port skipped, and it is the reason bulk writes stalled: the
     * Ext_RAM read path needs the DIN delay tuned for the module clock it is
     * actually running at.
     */
    psram_write_reference(1);
    tune = psram_tune();
    boot_trace_hex(tune);

    extmem_cache_suspend();
    mspi_enter_high_speed(tune);
    extmem_cache_resume();

    config_psram_spi_phases();

    /* ESP-IDF ends the bring-up with Cache_Resume_DCache(0): the Ext_RAM D-cache is
     * re-enabled so loads through a PSRAM-mapped MMU page are served by the cache
     * rather than rejected. The external-ram doc is explicit that the two are not
     * separable: "When flash cache is disabled ... the external RAM also becomes
     * inaccessible. Any read operations from or write operations to it will lead to
     * an illegal cache access exception." PSRAM is not a second memory bus; it is
     * the cache's other backing store.
     */
    boot_trace('5');
    { typedef void (*resume_t)(int);
      ((resume_t)ROM_CACHE_RESUME_DCACHE)(0); }

    boot_trace_hex(rd(EXTMEM_DCACHE_CTRL));
    boot_trace_hex(rd(SPI_SMEM_DIN_MODE(0)));
    boot_trace_hex(rd(SPI_SMEM_CALI(0)));
    boot_trace_hex(rd(SPI_SRAM_CLK(0)));
    boot_trace_hex(rd(SPI_CORE_CLK_SEL(0)));

    if (cache_psram_probe()) {
        s_psram_present = true;
        boot_trace('6');
        return true;
    }

    boot_trace('!');
    s_psram_present = false;
    return false;
}

#endif /* CPU_XTENSA */
