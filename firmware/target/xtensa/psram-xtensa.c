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
#include "bootlog.h"

#ifdef CPU_XTENSA

/* SPI_MEM instances: 0 is SPI0 (the cache/MMU path), 1 is SPI1. */
#define SPI_MEM_BASE(i)       (0x60003000u - (unsigned)(i) * 0x1000u)

/* MMU, duplicated from mmu-xtensa.c so the probe can arm a page by itself. */
#define MMU_TABLE_BASE        0x600C5000u
#define MMU_PAGE              0x10000u
#define MMU_INVALID           (1u << 14)
#define MMU_TARGET_PSRAM      (1u << 15)
#define FLASH_WINDOW_LOW      0x3C000000u

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
#define SPI_CACHE_FCTRL(i)    (SPI_MEM_BASE(i) + 0x3C)
#define SPI_SRAM_CMD(i)       (SPI_MEM_BASE(i) + 0x44)
#define SPI_SRAM_DRD(i)       (SPI_MEM_BASE(i) + 0x48)
#define SPI_SRAM_DWR(i)       (SPI_MEM_BASE(i) + 0x4C)
#define SPI_SRAM_CLK(i)       (SPI_MEM_BASE(i) + 0x50)
#define SPI_W0(i)             (SPI_MEM_BASE(i) + 0x58)
#define SPI_CORE_CLK_SEL(i)   (SPI_MEM_BASE(i) + 0xEC)
#define SPI_SMEM_AC(i)        (SPI_MEM_BASE(i) + 0xDC)
#define SPI_SMEM_CALI(i)      (SPI_MEM_BASE(i) + 0xBC)
#define SPI_SMEM_DIN_MODE(i)  (SPI_MEM_BASE(i) + 0xC0)
#define SPI_SMEM_DIN_NUM(i)   (SPI_MEM_BASE(i) + 0xC4)
#define SPI_FMEM_DDR(i)       (SPI_MEM_BASE(i) + 0xE0)
#define SPI_SMEM_DDR(i)       (SPI_MEM_BASE(i) + 0xE4)
#define SPI_CLK_GATE(i)       (SPI_MEM_BASE(i) + 0xE8)
#define SPI_DATE(i)           (SPI_MEM_BASE(i) + 0x3FC)

#define CMD_USR               (1u << 18)
#define SPI_FSM(i)            (SPI_MEM_BASE(i) + 0x54)
#define SPI_MEM_INT_RAW(i)    (SPI_MEM_BASE(i) + 0xF8)
#define SPI_FCTRL(i)          (SPI_MEM_BASE(i) + 0x3C)

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
/* Measured on this board: the octal read data only lines up with the FIFO word at
 * a dummy phase of 6. Longer dummy phases do not extend the read, they slide the
 * window - 18 gives 0x55555555, 8 gives 0x55555a6b, 6 gives the value that was just
 * written. The datasheet's 10-cycle latency is counted in single-line bits, not in
 * octal cycles (8 bits each), which is why the number looks so small.
 */
#define RD_DUMMY_BITS       6
#define WR_DUMMY_BITS       8
/* SPI0's Ext_RAM cache path is a different FSM from SPI1's user command path, and
 * it uses the latency Espressif documents for the Ext_RAM read phase.
 */
#define CACHE_RD_DUMMY      18
#define CACHE_WR_DUMMY      8
#define MR_DUMMY_BITS       8

/* Pad 26 is the SPICS1 pad in the IO MUX; the PSRAM chip select is not routed
 * through the GPIO matrix. */
#define IO_MUX_SPICS1       (DR_REG_IO_MUX_BASE + 0x6C)
#define IO_MUX_FUN_DRV_S    10

#define SYSCON_BASE         0x60026000
#define SYSCON_SRAM_ACE0    (SYSCON_BASE + 0x58)
#define SRAM_ECC_ENABLE     (1u << 8)

/* The MSPI core clock is 160 MHz (CORE_CLK_SEL 2), which is what ESP-IDF runs
 * it at. SPI1 stays at 20 MHz while the chip's mode registers are being
 * programmed; the flash path and the PSRAM data path both run at 80 MHz, which
 * is the rating of the octal PSRAM on the module.
 *
 * ESP-IDF gets to 80 MHz by sweeping dummy and delay values. Rockbox has no
 * equivalent sweep, so it uses the configuration ESP-IDF falls back to when the
 * sweep fails: input delay mode 4 on every DIN line and on DQS, zero delay
 * cycles, and two extra dummy cycles on the Ext_RAM read path. That is the entry
 * mspi_timing_tuning_configs.h names as default_config_id for a 160 MHz core
 * clock with an 80 MHz module clock in DTR mode, so it is a known-good setting
 * rather than a guess at one. */
#define FLASH_DIV           2      /* 80 MHz */
#define BRINGUP_DIV         8      /* 20 MHz */
#define PSRAM_DIV           2      /* 80 MHz */

/* DIN_MODE has nine 3-bit fields (DIN0..DIN7, then DQS); DIN_NUM has nine 2-bit
 * fields. Mode 4 is "delay (num+1) cycles at the HCLK negative edge and one
 * cycle at the MSPI core clock positive edge". */
#define DIN_MODE_VALUE      4u
#define DIN_NUM_VALUE       0u
#define PSRAM_EXTRA_DUMMY   2u

/* Chicken and egg: this whole path reconfigures the MSPI block, and the MSPI
 * block is what feeds the flash instruction-cache window that .text is executed
 * from. Slowing the flash clock to 20 MHz for the bring-up, and reprogramming the
 * Ext_RAM command/address/dummy phases while the CPU is still fetching its next
 * instruction out of that same window, is not a safe thing to do.
 *
 * So every function here is ICODE_ATTR: it runs from .iram, which the mask ROM
 * loader copies into internal SRAM before it jumps to the entry point. Internal
 * SRAM needs no MMU page, no cache and no flash read to be fetched.
 *
 * The same applies to boot_trace() and xt_rom_call(): a marker printed from
 * .text is a fetch from the window being reconfigured.
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

/* The input-delay and extra-dummy registers only matter once the Ext_RAM clock
 * is at its final speed, so this is paired with set_psram_clock(). */
static void ICODE_ATTR set_psram_timing(void)
{
    uint32_t mode = 0;
    uint32_t num = 0;

    for (int i = 0; i < 9; i++) {
        mode |= (DIN_MODE_VALUE & 7u) << (3 * i);
        num  |= (DIN_NUM_VALUE & 3u) << (2 * i);
    }

    wr(SPI_SMEM_DIN_MODE(0), mode);
    wr(SPI_SMEM_DIN_NUM(0), num);

    /* bit 1 enables the calibration dummy, bits 4:2 give its length */
    wr(SPI_SMEM_CALI(0), (1u << 1) | (PSRAM_EXTRA_DUMMY << 2));
}

/* One octal transaction on SPI1: CMD phase, ADDR phase, DUMMY phase, then
 * optionally DOUT and DIN. The data buffer is a little-endian byte array, so the
 * first byte to go out is in bits 0-7 of W0. */
static bool ICODE_ATTR opi_command(int spi, uint32_t cmd, int cmd_bits, uint32_t addr, int addr_bits,
                        int dummy_bits, uint32_t dout, int dout_bits,
                        uint32_t *din, int din_bits)
{
    int guard = 0;

    /* The controller is idle when CMD reads back zero. */
    while (rd(SPI_CMD(spi)) != 0 && guard++ < 10000)
        ;
    if (rd(SPI_CMD(spi)) != 0) {
        boot_trace('!');            /* SPI1 never went idle */
        return false;
    }

    wr(SPI_CTRL(spi), (rd(SPI_CTRL(spi)) &
                       ~(CTRL_FREAD_QIO | CTRL_FREAD_DIO |
                         CTRL_FREAD_QUAD | CTRL_FREAD_DUAL)) |
                      (CTRL_FCMD_OCT | CTRL_FADDR_OCT | CTRL_FDIN_OCT |
                       CTRL_FDOUT_OCT | CTRL_FDUMMY_OUT));

    /* CS1 is the PSRAM, CS0 is the flash. */
    wr(SPI_MISC(spi), (rd(SPI_MISC(spi)) & ~(MISC_CS0_DIS | MISC_CS1_DIS)) |
                      MISC_CS0_DIS);

    /* The ROM's spi_cmd_config does exactly this: a 32-bit address phase for a
     * SPI1 user command needs CACHE_USR_CMD_4BYTE set, otherwise the address phase
     * is not the length USER1 claims and the chip never sees a valid command.
     */
    if (addr_bits == 32)
        wr(SPI_CACHE_FCTRL(spi), rd(SPI_CACHE_FCTRL(spi)) | (1u << 1));
    else
        wr(SPI_CACHE_FCTRL(spi), rd(SPI_CACHE_FCTRL(spi)) & ~((1u << 1) | (1u << 0)));

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
    if (rd(SPI_CMD(spi)) != 0) {
        boot_trace('?');            /* a transaction never completed */
        return false;
    }

    if (din)
        *din = rd(SPI_W0(spi));

    return true;
}

static void ICODE_ATTR init_psram_pins(void)
{
    /* CS1, HD, WP, CLK, Q0-Q3, D0-D3 all live on the IO MUX, so only the pad
     * function and drive strength need touching. ESP-IDF uses the strongest
     * driver on every one of them; pads 29 (CS0) and 30 (SPICLK) are in the list
     * too, though the clock pad's drive is overridden by SPI_MEM_DATE_REG below.
     */
    boot_trace('a');
    uint32_t cs1 = rd(IO_MUX_SPICS1);
    boot_trace('b');
    wr(IO_MUX_SPICS1, (cs1 & ~((7u << 12) | (3u << 10))) | (3u << 10));
    boot_trace('c');

    /* Octal needs all eight data pads. D0..D3 are SPID(32), SPIQ(31), SPIHD(27)
     * and SPIWP(28); pads 27 and 28 come out of reset in GPIO mode because this
     * module's flash is quad, so two of the eight lines are missing and the chip
     * never sees a complete command. Force every MSPI pad to its SPI function.
     */
    for (int pin = 26; pin <= 32; pin++)
        wr(IO_MUX_PIN(pin), (rd(IO_MUX_PIN(pin)) & ~((7u << 12) | (3u << 10))) |
                             (3u << 10));
    for (int pin = 26; pin <= 32; pin++)
        boot_trace_hex(rd(IO_MUX_PIN(pin)));
    boot_trace('d');

    /* Octal mode needs eight data lines. D0-D3 are SPID/SPIQ/SPIHD/SPIWP (pads
     * 32, 31, 27, 28); D4-D7 and DQS are the SPIIO4..7 / SPIDQS functions on pads
     * 33..37. The ROM routes those only when the efuses describe an OPI *flash*,
     * which is not the case on a quad-flash module with octal PSRAM, so they come
     * back in GPIO mode and the chip sees four of its eight lines.
     */
    for (int pin = 33; pin <= 37; pin++)
        wr(IO_MUX_PIN(pin), (rd(IO_MUX_PIN(pin)) & ~((7u << 12) | (3u << 10))) |
                             (4u << 12) | (3u << 10));
    for (int pin = 33; pin <= 37; pin++)
        boot_trace_hex(rd(IO_MUX_PIN(pin)));

    /* The SPI clock pad's drive strength is controlled from the SPI block
     * itself, not the IO MUX: enable the override, then set both the Ext_RAM and
     * flash-side clock pads to the strongest driver. */
    wr(SPI_DATE(0), rd(SPI_DATE(0)) | (1u << 4) | 0x3u | (3u << 2));
    boot_trace('e');
}

/*
 * Second route to the chip: SPI0's Ext_RAM cache path. The hardware builds the
 * octal transaction itself (command, address, dummy, data) from CACHE_SCTRL,
 * SRAM_DRD, SRAM_DWR and SRAM_CMD, so a read through a PSRAM-mapped MMU page
 * exercises the same pins and the same chip without any of our hand-built SPI1
 * register sequencing. If this works and the SPI1 user command does not, the
 * transaction is the bug; if it fails the same way, the chip is not answering.
 */
static void ICODE_ATTR psram_cache_test(void)
{
    volatile uint32_t *mmu = (volatile uint32_t *)MMU_TABLE_BASE;
    uint32_t v;
    int guard;


    /* Dump the Ext_RAM path before configuring it, so a missing bit is visible
     * rather than guessed at.
     */
    boot_trace('R');
    for (int i = 0; i < 16; i++)
        boot_trace_hex(rd(0x600C4000u + 4u * (unsigned)i));
    /* Cache bridge / MMU fault state: the Ext_RAM cache keeps the flash address
     * range it is allowed to serve, and it latches a fault record when an MMU entry
     * is not usable. Reading those tells us whether the PSRAM access is being
     * rejected before the SPI transaction even starts.
     */
    boot_trace('X');
    for (int i = 0; i < 8; i++)
        boot_trace_hex(rd(0x600C40B4u + 4u * (unsigned)i));
    boot_trace_hex(rd(0x600C4120u));
    boot_trace_hex(rd(0x600C4124u));
    boot_trace_hex(rd(0x600C4130u));
    /* Freeze/owner/misc: a frozen Ext_RAM cache retries every access forever, which
     * looks exactly like the stall we are seeing.
     */
    boot_trace('Y');
    for (int i = 0; i < 8; i++)
        boot_trace_hex(rd(0x600C4128u + 4u * (unsigned)i));
    {
        static const uint32_t s0regs[] = {
            SPI_CTRL(0), SPI_CLOCK(0), SPI_CACHE_FCTRL(0), SPI_CACHE_SCTRL(0),
            SPI_SRAM_CMD(0), SPI_SRAM_DRD(0), SPI_SRAM_DWR(0), SPI_SRAM_CLK(0),
            SPI_SMEM_AC(0), SPI_SMEM_CALI(0), SPI_SMEM_DIN_MODE(0),
            SPI_SMEM_DIN_NUM(0), SPI_SMEM_DDR(0), SPI_CORE_CLK_SEL(0),
            SPI_MISC(0), SPI_CMD(0), SPI_ADDR(0), SPI_USER(0),
        };
        for (unsigned i = 0; i < sizeof s0regs / sizeof s0regs[0]; i++) {
            boot_trace((char)('a' + (int)i));
            boot_trace_hex(rd(s0regs[i]));
        }
    }

    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) &
                            ~((0x3Fu << 22) | (0x3Fu << 14) | (0x3Fu << 6))) |
                           SCTRL_USR_WCMD | SCTRL_USR_RCMD | SCTRL_SCMD_4BYTE |
                           SCTRL_USR_RD_DUMMY | SCTRL_USR_WR_DUMMY | SCTRL_SRAM_OCT |
                           ((CACHE_RD_DUMMY - 1u) << 6) |
                           ((CACHE_WR_DUMMY - 1u) << 22) |
                           (31u << 14));
    wr(SPI_SRAM_DRD(0), (15u << 28) | OPI_SYNC_READ);
    wr(SPI_SRAM_DWR(0), (15u << 28) | OPI_SYNC_WRITE);
    wr(SPI_SMEM_DDR(0), (rd(SPI_SMEM_DDR(0)) & ~(DDR_RDAT_SWP | DDR_WDAT_SWP)) |
                         DDR_EN | DDR_VAR_DUMMY);
    wr(SPI_SRAM_CMD(0), rd(SPI_SRAM_CMD(0)) | SCMD_SDUMMY_OUT | SCMD_SCMD_OCT |
                        SCMD_SADDR_OCT | SCMD_SDOUT_OCT | SCMD_SDIN_OCT);

    /* The table indexes the low 25 bits of the address, and the data bus window is
     * 0x3C000000..0x3DFFFFFF, so entry e is simply 0x3C000000 + e * 64 KB.
     */
    /* Sweep. Every row is one Ext_RAM configuration; the CPU read is the only
     * thing here that can stall, so the marker written immediately before it names
     * the config that fails. The first row is the control: a flash-target entry the
     * cache has never touched, which must come back.
     */
    wr(0x600C4148u, rd(0x600C4148u) | 0x2u);                       /* MMU owner: dcache */
    wr(SPI_MISC(1), (rd(SPI_MISC(1)) & ~MISC_CS1_DIS) | MISC_CS0_DIS);
    boot_trace('O');
    xt_rom_call(0x4004f64cu, 0, 0);           /* Cache_Owner_Init */
    boot_trace_hex(rd(0x600C4148u));          /* MMU owner bitmap */
    xt_rom_call(0x4004f480u, 0, 0);          /* Cache_Resume_DCache(0) */
    boot_trace_hex(rd(0x600C4000u));         /* DCACHE_CTRL */
    boot_trace_hex(rd(0x600C4004u));         /* DCACHE_CTRL1 */

    /* Seed the PSRAM pages the probe will read, through SPI1. */
    boot_trace('S');
    for (unsigned p = 0; p < 4; p++) {
        unsigned page = 103u + p;
        if (opi_command(1, OPI_SYNC_WRITE, CMD_BITS, page * MMU_PAGE, ADDR_BITS,
                        WR_DUMMY_BITS, 0xcafe0000u + page, 32, NULL, 0))
            boot_trace((char)('0' + (int)page % 10));
        else
            boot_trace('!');
    }

    /* Control: entry 103 mapped to flash page 1. That page holds real rodata, so a
     * non-0xFFFFFFFF answer proves the D-cache consults the MMU we are writing.
     */
    mmu[103] = 1u;
    xt_rom_call(0x4004eac4u, 0, 0);       /* Cache_Invalidate_DCache_All */
    boot_trace('a');
    boot_trace_hex(mmu[103]);
    v = *(volatile uint32_t *)(0x3C000000u + 103u * MMU_PAGE);
    boot_trace_hex(v);
    boot_trace_hex(rd(SPI_FSM(0)));           /* SPI0 FSM after the fetch */

    /* The synchronous read of a PSRAM page stalls the core, so ask the cache to do
     * the same fetch asynchronously and watch the MSPI state machine while it is in
     * flight. That separates "the transaction never starts" from "the chip does not
     * answer".
     */
    set_psram_clock(2);
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) &
                            ~((0x3Fu << 22) | (0x3Fu << 6))) |
                           ((CACHE_RD_DUMMY - 1u) << 6) |
                           ((CACHE_WR_DUMMY - 1u) << 22));
    wr(SPI_MISC(0), rd(SPI_MISC(0)) & ~(1u << 8));
    mmu[103] = 103u | MMU_TARGET_PSRAM;
    boot_trace('P');
    boot_trace_hex(mmu[103]);
    boot_trace_hex(rd(SPI_SRAM_CLK(0)));      /* Ext_RAM clock */
    boot_trace_hex(rd(SPI_CACHE_SCTRL(0)));   /* Ext_RAM phases */
    boot_trace_hex(rd(SPI_MISC(0)));          /* SSUB_PIN is bit 8 */
    wr(0x600C40DCu, 0x3Fu);                   /* enable every illegal-op fault */
    wr(0x600C40E0u, 0x3Fu);                   /* clear any latched fault */
    wr(0x600C4044u, 0x3C670000u);
    wr(0x600C4048u, 16u);
    wr(0x600C4040u, 1u);                      /* PRELOAD_ENA */
    guard = 0;
    while (!(rd(0x600C4040u) & 2u) && guard++ < 300000)
        ;
    boot_trace_hex(rd(0x600C4040u));          /* bit1 = DONE */
    boot_trace_hex(guard);                    /* iterations spent waiting */
    boot_trace_hex(rd(SPI_FSM(0)));           /* SPI0 flash/Ext_RAM FSM */
    boot_trace_hex(rd(SPI_SRAM_CMD(0)));      /* Ext_RAM octal phases */
    boot_trace_hex(rd(SPI_SRAM_DRD(0)));      /* Ext_RAM read command */
    boot_trace_hex(rd(0x600C4130u));          /* icache/dcache FSM state */
    boot_trace_hex(rd(0x600C40E4u));          /* illegal-op status */
    boot_trace_hex(rd(0x600C4120u));          /* MMU fault */
    boot_trace_hex(rd(SPI_MEM_INT_RAW(0)));   /* SPI0 interrupt raw */
    boot_trace_hex(rd(SPI_MEM_INT_RAW(1)));   /* SPI1 interrupt raw */
    boot_trace_hex(rd(SPI_W0(1)));            /* SPI1 FIFO, last user command */
    boot_trace('Q');
    /* Preload is bounded, so it can be swept. Every row is one Ext_RAM clock and
     * read-dummy length; the answer is the cache line after the fetch. 0xcafe0067
     * is what SPI1 put in that page, so the row that returns it is the one where
     * SPI0's Ext_RAM FSM actually talked to the chip.
     */
    /* Preload is bounded, so it can be swept. First release SPI1: the MSPI block
     * has one bus and an interrupt source named "SPI0 Cache access and SPI1 access
     * rejected" says the two are mutually exclusive. If SPI1 still has CS1 enabled
     * and its octal phases set, SPI0's Ext_RAM FSM may never get the bus, which is
     * exactly what a stalled preload looks like.
     */
    boot_trace('L');
    wr(SPI_CTRL(1), rd(SPI_CTRL(1)) & ~(CTRL_FCMD_OCT | CTRL_FADDR_OCT |
                                        CTRL_FDIN_OCT | CTRL_FDOUT_OCT));
    wr(SPI_MISC(1), rd(SPI_MISC(1)) | MISC_CS0_DIS | MISC_CS1_DIS);
    wr(SPI_CLK_GATE(1), rd(SPI_CLK_GATE(1)) & ~1u);
    boot_trace_hex(rd(SPI_CLK_GATE(0)));
    boot_trace_hex(rd(SPI_CLK_GATE(1)));
    boot_trace_hex(rd(SPI_CTRL(1)));

    static const unsigned char divs[] = {2, 4, 8};
    static const unsigned char rds[] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20};
    boot_trace('W');
    for (unsigned d = 0; d < sizeof divs / sizeof divs[0]; d++) {
        for (unsigned r = 0; r < sizeof rds / sizeof rds[0]; r++) {
            set_psram_clock(divs[d]);
            wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~((0x3Fu << 6) |
                                                                (0x3Fu << 22))) |
                                   ((rds[r] - 1u) << 6) | ((CACHE_WR_DUMMY - 1u) << 22));
            xt_rom_call(0x4004eac4u, 0, 0);       /* Cache_Invalidate_DCache_All */
            wr(0x600C4044u, 0x3C670000u);
            wr(0x600C4048u, 16u);
            wr(0x600C4040u, 1u);
            guard = 0;
            while (!(rd(0x600C4040u) & 2u) && guard++ < 200000)
                ;
            if (!(rd(0x600C4040u) & 2u)) {
                boot_trace('!');
                boot_trace_hex(rd(SPI_FSM(0)));
                boot_trace_hex(rd(SPI_MEM_INT_RAW(0)));
                boot_trace_hex(rd(0x600C4130u));   /* cache state while stalled */
                /* The ROM's Cache_End_DCache_Preload waits for DONE, which never
                 * arrives, so clear the enable bit by hand instead.
                 */
                wr(0x600C4040u, 0u);
                continue;
            }
            boot_trace((char)('A' + (int)d));
            boot_trace((char)('a' + (int)r));
            boot_trace_hex(rd(SPI_SRAM_CLK(0)));
            v = *(volatile uint32_t *)0x3C670000u;
            boot_trace_hex(v);
        }
    }

    /* Why the fetch stalls: the Ext_RAM block latches the rejected bus access and
     * the MMU fault that caused it. */
    boot_trace('Z');
    for (unsigned i = 0x100; i <= 0x130; i += 4)
        boot_trace_hex(rd(0x600C4000u + i));
    boot_trace_hex(rd(SPI_FSM(0)));
    boot_trace_hex(rd(SPI_MEM_INT_RAW(0)));
    boot_trace_hex(rd(SPI_MEM_BASE(0) + 0xFC));
    boot_trace_hex(rd(SPI_MEM_BASE(1) + 0xF8));
}

/* Bring-up probe: the first connectivity check comes back all ones, which is what
 * an undriven data bus reads as. Sweep the dummy-phase length, because the chip's
 * read latency is the only thing that decides when the data lines are driven, and
 * dump the mode register so the chip's own answer is visible.
 */
/* Control experiment: can the user-command path drive the MSPI pads at all? A plain
 * single-line READ of flash offset 0 exercises CMD/ADDR/DIN on the same FIFO, against
 * a device we know is there. Expect 0xe9000103 (the ESP image header).
 */
static void ICODE_ATTR flash_read_test(int spi);
/* Run one OPI transaction through the ROM's own command builder, so the register
 * sequence is Espressif's rather than ours. If the ROM can talk to the chip and we
 * cannot, the difference is the bug.
 */
extern void rom_opi_exec(const uint32_t *args);

static void ICODE_ATTR rom_opi_transaction(int spi, uint32_t cmd, int cmd_bits,
                                          uint32_t addr, int addr_bits, int dummy_bits,
                                          uint32_t dout, int dout_bits,
                                          uint32_t *din, int din_bits)
{
    uint32_t args[13];

    args[0] = (uint32_t)spi;
    args[1] = 7;                        /* ESP_ROM_SPIFLASH_OPI_DTR_MODE */
    args[2] = cmd;
    args[3] = (uint32_t)cmd_bits;
    args[4] = addr;
    args[5] = (uint32_t)addr_bits;
    args[6] = (uint32_t)dummy_bits;
    args[7] = dout_bits ? (uint32_t)&dout : 0;
    args[8] = (uint32_t)dout_bits;
    args[9] = din ? (uint32_t)din : 0;
    args[10] = (uint32_t)din_bits;
    args[11] = 2;                       /* cs_mask: CS1 */
    args[12] = 0;
    rom_opi_exec(args);
}

static void ICODE_ATTR psram_probe(void)
{
    static const unsigned char dummies[] = {0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 16, 18, 20};
    uint32_t ref = 0x5a6b7c8d, got = 0, mr0 = 0;
    int ok = 0;
    uint32_t regs[18];

    regs[0] = SPI_CMD(1);      regs[1] = SPI_CTRL(1);   regs[2] = SPI_MEM_BASE(1) + 0x0C;
    regs[3] = SPI_CLOCK(1);    regs[4] = SPI_USER(1);   regs[5] = SPI_USER1(1);
    regs[6] = SPI_USER2(1);    regs[7] = SPI_MOSI_DLEN(1); regs[8] = SPI_MISO_DLEN(1);
    regs[9] = SPI_MISC(1);     regs[10] = SPI_FSM(1);   regs[11] = SPI_FMEM_DDR(1);
    regs[12] = SPI_SMEM_DDR(1); regs[13] = SPI_SMEM_AC(1); regs[14] = SPI_SMEM_DIN_MODE(1);
    regs[15] = SPI_CORE_CLK_SEL(1); regs[16] = SPI_SRAM_CLK(1); regs[17] = SPI_W0(1);

    boot_trace('P');
    boot_trace_hex(rd(IO_MUX_SPICS1));
    /* SPI0 is the cache path: its CTRL FREAD_* bits drive the flash read FSM that
     * the instruction and data caches use. A user command on SPI1 exercises the same
     * machinery against a device we know is there without disturbing the cache, so
     * SPI0 is left alone. (Running the test on SPI0 clears its FREAD_* bits and every
     * later fetch from the IROM window returns all ones.)
     */
    flash_read_test(1);

    for (unsigned i = 0; i < 18; i++) {
        boot_trace((char)('0' + i));
        boot_trace_hex(rd(regs[i]));
    }

    if (opi_command(1, OPI_REG_READ, CMD_BITS, 0x0, ADDR_BITS, 8, 0, 0, &mr0, 16))
        boot_trace_hex(mr0);
    else
        boot_trace('M');

    /* Dump the whole FIFO after one write/read pair. If the read data is not in W0
     * the octal FIFO packs differently than the ROM's 4-bytes-per-word assumption,
     * and every read we have done so far has been reading the wrong register.
     */
    boot_trace('F');
    if (opi_command(1, OPI_SYNC_WRITE, CMD_BITS, 0x0, ADDR_BITS, 8,
                    ref, 32, NULL, 0) &&
        opi_command(1, OPI_SYNC_READ, CMD_BITS, 0x0, ADDR_BITS, 18, 0, 0, &got, 32)) {
        for (int i = 0; i < 8; i++)
            boot_trace_hex(rd(SPI_MEM_BASE(1) + 0x58 + 4 * i));
        boot_trace_hex(rd(SPI_MEM_BASE(1) + 0x30));   /* EXT_ADDR */
        boot_trace_hex(rd(SPI_FSM(1)));
    } else {
        boot_trace('f');
    }

    /* Sweep the dummy phase on both SPI instances. If the chip is selected and
     * listening, some latency value must produce a non-floating read.
     */
    for (int spi = 1; spi < 2; spi++) {
        boot_trace('S' + spi);
        for (unsigned i = 0; i < sizeof dummies; i++) {
            got = 0;
            if (!opi_command(spi, OPI_SYNC_WRITE, CMD_BITS, 0x0, ADDR_BITS, 8,
                             ref, 32, NULL, 0)) {
                boot_trace('w');
                continue;
            }
            if (!opi_command(spi, OPI_SYNC_READ, CMD_BITS, 0x0, ADDR_BITS, dummies[i],
                             0, 0, &got, 32)) {
                boot_trace('r');
                continue;
            }
            boot_trace_hex(got);
            if (got == ref)
                ok = 1;
        }
    }

    /* Only worth trying the cache path once the chip has answered a hand-built
     * transaction: an Ext_RAM read against a chip that is not answering stalls the
     * MSPI FSM and the boot with it.
     */
    if (ok)
        psram_cache_test();
}

/* Control experiment: can SPI1 talk to the flash at all? A plain single-line READ
 * of flash offset 0 through SPI1 with CS0 selected exercises the same user-command
 * path (CMD/ADDR/DIN phases, the same FIFO) but against a device we know is there.
 * If this returns the image header, the SPI1 transaction machinery is fine and the
 * problem is specific to CS1/the PSRAM; if it returns all ones too, the transaction
 * is not running at all.
 */
static void ICODE_ATTR flash_read_test(int spi)
{
    int guard = 0;

    boot_trace('C' + spi);
    wr(SPI_CTRL(spi), rd(SPI_CTRL(spi)) & ~(CTRL_FREAD_QIO | CTRL_FREAD_DIO |
                                            CTRL_FREAD_QUAD | CTRL_FREAD_DUAL |
                                            CTRL_FCMD_OCT | CTRL_FADDR_OCT |
                                            CTRL_FDIN_OCT | CTRL_FDOUT_OCT));
    wr(SPI_MISC(spi), (rd(SPI_MISC(spi)) & ~(MISC_CS0_DIS | MISC_CS1_DIS)) | MISC_CS1_DIS);
    wr(SPI_ADDR(spi), 0);
    wr(SPI_USER1(spi), (23u << 26));
    wr(SPI_USER2(spi), (7u << 28) | 0x03u);
    wr(SPI_USER(spi), USER_COMMAND | USER_ADDR | USER_MISO);
    wr(SPI_MISO_DLEN(spi), 31);
    wr(SPI_CMD(spi), CMD_USR);
    while (rd(SPI_CMD(spi)) != 0 && guard++ < 100000)
        ;
    boot_trace_hex(rd(SPI_W0(spi)));
    boot_trace_hex(rd(SPI_CMD(spi)));
}


bool ICODE_ATTR psram_present(void)
{
    return s_psram_present;
}

bool ICODE_ATTR psram_init(void)
{
    boot_trace('p');                /* PSRAM bring-up started */
    /* SPI0 and SPI1 share the core clock selection, and they also share the flash
     * read FSM: ESP-IDF's own comment on mspi_timing_enter_low_speed_mode() warns
     * that slowing SPI1 alone breaks SPI0 if SPI0 is still running fast. So the
     * flash path is slowed for the duration of the bring-up and restored at the
     * end, rather than left at its final speed while SPI1 talks octal. */
    wr(SPI_CORE_CLK_SEL(0), 2);                /* 160 MHz */
    set_spi_clock(0, BRINGUP_DIV);
    set_spi_clock(1, BRINGUP_DIV);
    boot_trace('1');

    init_psram_pins();
    boot_trace('2');

    /* CS setup/hold for the Ext_RAM path. The chip needs a minimum CS high time
     * between transactions; the reset defaults are too short for octal mode. */
    boot_trace('3');
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
    boot_trace('4');
    wr(SYSCON_SRAM_ACE0, rd(SYSCON_SRAM_ACE0) & ~SRAM_ECC_ENABLE);

    /* Variable dummy length on SPI1's read path; the fixed default is not right
     * for the PSRAM's latency. */
    wr(SPI_FMEM_DDR(1), rd(SPI_FMEM_DDR(1)) | DDR_VAR_DUMMY);
    /* No DTR byte swap: that is the reset state, and ESP-IDF's ROM call with
     * (false, false) amounts to the same thing. */
    wr(SPI_FMEM_DDR(1), rd(SPI_FMEM_DDR(1)) & ~(DDR_WDAT_SWP | DDR_RDAT_SWP));

    /* MR0: fixed latency (lt = 1, bit 5), read latency 2 (bits 4:2), drive
     * strength 0 (bits 1:0). Only the low 16 bits of the data phase are the
     * mode register. */
    boot_trace('4');
    if (!opi_command(1, OPI_REG_WRITE, CMD_BITS, 0x0, ADDR_BITS, 0,
                     (1u << 5) | (2u << 2), 16, NULL, 0))
        return false;
    boot_trace('5');

    /* MR8: burst length 3, burst type 0. The chip's power-on default is the
     * same, but ESP-IDF writes it, and relying on a reset value that a previous
     * firmware image may have changed is the kind of thing that shows up only on
     * a board that has been flashed before. */
    if (!opi_command(1, OPI_REG_WRITE, CMD_BITS, 0x8, ADDR_BITS, 0,
                     3u, 16, NULL, 0))
        return false;
    boot_trace('6');

    /* Prove the chip is there *and* addressed: write one word, read it, read an
     * address that was never written, read the first address again. A floating bus
     * or a FIFO that just echoes the last transaction would pass a single read.
     */
    uint32_t a = 0x11223344u, x = 0, y = 0, z = 0;

    if (!opi_command(1, OPI_SYNC_WRITE, CMD_BITS, 0x1000, ADDR_BITS, WR_DUMMY_BITS,
                     a, 32, NULL, 0)) {
        boot_trace('w');
        return false;
    }

    boot_trace('7');
    if (opi_command(1, OPI_SYNC_READ, CMD_BITS, 0x1000, ADDR_BITS, RD_DUMMY_BITS,
                    0, 0, &x, 32))
        boot_trace_hex(x);
    if (opi_command(1, OPI_SYNC_READ, CMD_BITS, 0x20000, ADDR_BITS, RD_DUMMY_BITS,
                    0, 0, &y, 32))
        boot_trace_hex(y);
    if (opi_command(1, OPI_SYNC_READ, CMD_BITS, 0x1000, ADDR_BITS, RD_DUMMY_BITS,
                    0, 0, &z, 32))
        boot_trace_hex(z);

    if (x != a || z != a || y == a) {
        boot_trace('x');            /* chip answered, but not per address */
        psram_probe();
        return false;
    }

    /* Now SPI0 can reach the chip. Command, address and dummy phases all octal,
     * 32-bit address, and the chip's own read/write command words. */
    wr(SPI_CACHE_SCTRL(0), rd(SPI_CACHE_SCTRL(0)) | SCTRL_USR_WCMD | SCTRL_USR_RCMD |
                           SCTRL_SCMD_4BYTE | SCTRL_USR_RD_DUMMY | SCTRL_USR_WR_DUMMY |
                           SCTRL_SRAM_OCT);

    wr(SPI_SRAM_DWR(0), (15u << 28) | OPI_SYNC_WRITE);
    wr(SPI_SRAM_DRD(0), (15u << 28) | OPI_SYNC_READ);
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 14)) | (31u << 14));
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 6)) |
                           ((CACHE_RD_DUMMY - 1) << 6));
    wr(SPI_CACHE_SCTRL(0), (rd(SPI_CACHE_SCTRL(0)) & ~(0x3Fu << 22)) |
                           ((CACHE_WR_DUMMY - 1) << 22));

    wr(SPI_SMEM_DDR(0), (rd(SPI_SMEM_DDR(0)) & ~(DDR_RDAT_SWP | DDR_WDAT_SWP)) |
                        DDR_EN | DDR_VAR_DUMMY);
    wr(SPI_SRAM_CMD(0), rd(SPI_SRAM_CMD(0)) | SCMD_SDUMMY_OUT | SCMD_SCMD_OCT |
                        SCMD_SADDR_OCT | SCMD_SDOUT_OCT | SCMD_SDIN_OCT);

    set_psram_clock(PSRAM_DIV);
    set_psram_timing();
    set_spi_clock(0, FLASH_DIV);

    boot_trace('8');
    psram_cache_test();
    boot_trace('9');
    s_psram_present = true;
    return true;
}

#endif /* CPU_XTENSA */
