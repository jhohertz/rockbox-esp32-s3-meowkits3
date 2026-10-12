/***************************************************************************
 * SDHOST controller for the ESP32-S3 (Synopsys DesignWare MMC IP block).
 *
 * Register offsets and bit positions are taken from
 * components/soc/esp32s3/register/soc/sdmmc_struct.h and cross-checked against
 * the bare-metal SVD-derived header; the command and descriptor handling follows
 * ESP-IDF's sd_host/sd_trans implementation, which is what the reference firmware
 * on this board uses.
 *
 * The board has one SD slot wired in 1-bit mode: CLK = GPIO47, CMD = GPIO48,
 * D0 = GPIO21. There is no card-detect pin, so the drive is treated as
 * non-removable and a missing card simply shows up as a command timeout.
 *
 * Data never goes through the FIFO: the IDMAC is the only data path, so every
 * transfer is described by a ring of 16-byte descriptors. The ring is closed back
 * onto the first descriptor, and the IDMAC suspends when it reaches one whose
 * OWNER bit is clear - which is how completion is detected.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "semaphore.h"
#include "sdmmc_host.h"
#include "xtensa.h"
#include "bootlog.h"
#include "gpio-xtensa.h"
#include <string.h>

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/*
 * The SDHOST clock generator takes either the 40 MHz XTAL or the 160 MHz BBPLL
 * and divides it by 1..16; each slot then has a further divider of 0 (bypass) or
 * 2*n for n = 1..255.
 *
 * XTAL is the only safe source. The BBPLL has to be powered up - RTC_CNTL_OPTIONS0
 * BBPLL_FORCE_PD cleared - before PLL160M can be used, and nothing guarantees that
 * happened: we inherit ESP-IDF's second-stage bootloader, which does not run the
 * application clock init. The crystal is always on.
 *
 * From 40 MHz the reachable rates are 400 kHz exactly (host 10, card 5) and 20 MHz
 * for the "25 MHz" slot. 50 MHz is not reachable at all, so the target config does
 * not advertise it. Running the bus below the card's rating is always legal.
 */

#define SDMMC_CTRL        (XT_SDMMC_BASE + 0x00)
#define SDMMC_PWREN       (XT_SDMMC_BASE + 0x04)
#define SDMMC_CLKDIV      (XT_SDMMC_BASE + 0x08)
#define SDMMC_CLKSRC      (XT_SDMMC_BASE + 0x0c)
#define SDMMC_CLKENA      (XT_SDMMC_BASE + 0x10)
#define SDMMC_TMOUT       (XT_SDMMC_BASE + 0x14)
#define SDMMC_CTYPE       (XT_SDMMC_BASE + 0x18)
#define SDMMC_BLKSIZ      (XT_SDMMC_BASE + 0x1c)
#define SDMMC_BYTCNT      (XT_SDMMC_BASE + 0x20)
#define SDMMC_INTMASK     (XT_SDMMC_BASE + 0x24)
#define SDMMC_CMDARG      (XT_SDMMC_BASE + 0x28)
#define SDMMC_CMD         (XT_SDMMC_BASE + 0x2c)
#define SDMMC_RESP(n)     (XT_SDMMC_BASE + 0x30 + 4u * (n))
#define SDMMC_MINTSTS     (XT_SDMMC_BASE + 0x40)
#define SDMMC_RINTSTS     (XT_SDMMC_BASE + 0x44)
#define SDMMC_STATUS      (XT_SDMMC_BASE + 0x48)
#define SDMMC_CDETECT     (XT_SDMMC_BASE + 0x50)
#define SDMMC_RST_N       (XT_SDMMC_BASE + 0x78)
#define SDMMC_BMOD        (XT_SDMMC_BASE + 0x80)
#define SDMMC_PLDMND      (XT_SDMMC_BASE + 0x84)
#define SDMMC_DBADDR      (XT_SDMMC_BASE + 0x88)
#define SDMMC_IDSTS       (XT_SDMMC_BASE + 0x8c)
#define SDMMC_IDINTEN     (XT_SDMMC_BASE + 0x90)
#define SDMMC_USRID       (XT_SDMMC_BASE + 0x68)
#define SDMMC_VERID       (XT_SDMMC_BASE + 0x6c)
#define SDMMC_HCON        (XT_SDMMC_BASE + 0x70)
#define SDMMC_FIFOTH      (XT_SDMMC_BASE + 0x4c)
#define SDMMC_CLK         (XT_SDMMC_BASE + 0x800)

/* CTRL */#define CTRL_RESET        (1u << 0)
#define CTRL_FIFO_RESET   (1u << 1)
#define CTRL_DMA_RESET    (1u << 2)
#define CTRL_INT_ENABLE   (1u << 4)
#define CTRL_DMA_ENABLE   (1u << 5)
#define CTRL_USE_IDMAC    (1u << 25)   /* use_internal_dma, not bit 21 */

/* BMOD */
#define BMOD_SW_RESET     (1u << 0)
#define BMOD_FIXED_BURST  (1u << 1)
#define BMOD_ENABLE       (1u << 7)

/* RINTSTS / INTMASK */
#define INT_CD            (1u << 0)
#define INT_RESP_ERR      (1u << 1)
#define INT_CMD_DONE      (1u << 2)
#define INT_DATA_OVER     (1u << 3)
#define INT_TXDR          (1u << 4)
#define INT_RXDR          (1u << 5)
#define INT_RCRC          (1u << 6)
#define INT_DCRC          (1u << 7)
#define INT_RTO           (1u << 8)
#define INT_DTO           (1u << 9)
#define INT_HTO           (1u << 10)
#define INT_FRUN          (1u << 11)
#define INT_HLE           (1u << 12)
#define INT_SBE           (1u << 13)
#define INT_ACD           (1u << 14)
#define INT_EBE           (1u << 15)

#define CMD_ERR_MASK      (INT_RESP_ERR | INT_RCRC | INT_RTO)
#define DATA_ERR_MASK     (INT_DTO | INT_DCRC | INT_HTO | INT_SBE | INT_EBE | INT_FRUN)

/* IDSTS / IDINTEN */
#define DMA_TI            (1u << 0)
#define DMA_RI            (1u << 1)
#define DMA_FBE           (1u << 2)
#define DMA_DU            (1u << 4)
#define DMA_CES           (1u << 5)
#define DMA_NI            (1u << 8)
#define DMA_AI            (1u << 9)

#define DMA_ERR_MASK      (DMA_FBE | DMA_DU | DMA_CES)
#define DMA_DONE_MASK     (DMA_TI | DMA_RI | DMA_NI)

/* STATUS: fifo watermarks/empty/full occupy bits 3:0, cmd_fsm_state bits 7:4,
 * then data3_status at 8 and data_busy at 9. */
#define STATUS_DATA_BUSY  (1u << 9)

/* CMD register */
#define CMD_INDEX_SHIFT   0
#define CMD_RESP_EXPECT   (1u << 6)
#define CMD_RESP_LONG     (1u << 7)
#define CMD_CHECK_CRC     (1u << 8)
#define CMD_DATA_EXPECTED (1u << 9)
#define CMD_READ_WRITE    (1u << 10)
#define CMD_STREAM        (1u << 11)
#define CMD_AUTO_STOP     (1u << 12)
#define CMD_WAIT_PRVDATA  (1u << 13)
#define CMD_STOP_ABORT    (1u << 14)
#define CMD_SEND_INIT     (1u << 15)
#define CMD_CARD_NUM_SHIFT 16
#define CMD_UPDATE_CLK    (1u << 21)
#define CMD_USE_HOLD      (1u << 29)
#define CMD_START         (1u << 31)

/* IDMAC descriptor: four 32-bit words. */
#define DESC_LAST         (1u << 2)
#define DESC_FIRST        (1u << 3)
#define DESC_CHAIN        (1u << 4)
#define DESC_END_RING     (1u << 5)
#define DESC_CES          (1u << 30)
#define DESC_OWNER        (1u << 31)

#define DESC_BUF_BYTES    4096    /* matches ESP-IDF's SDMMC_DMA_MAX_BUF_LEN */
#define MAX_NR_BLOCKS     128
#define DESC_COUNT        (MAX_NR_BLOCKS * SD_BLOCK_SIZE / DESC_BUF_BYTES)

struct sdmmc_desc
{
    uint32_t flags;
    uint32_t size;
    uint32_t buffer;
    uint32_t next;
};

struct sdmmc_ctl
{
    uint32_t bus_clock;       /* the SDMMC_BUS_CLOCK_* constant last requested */
    bool powered;
    int resp_len;
    volatile uint32_t events;     /* SDHOST interrupts seen since the last command */
    volatile uint32_t dma_events; /* IDMAC interrupts seen since the last command */
    struct sdmmc_desc desc[DESC_COUNT];
};

static struct sdmmc_host sdmmc_host;
static struct sdmmc_ctl sdmmc_ctl;
static struct semaphore sdmmc_sem;

/*
 * Two-stage divider. host_div is the clock generator (1..16), card_div is the
 * slot divider where 0 means bypass and n means 2*n.
 */
static void set_clock(uint32_t clock)
{
    uint32_t freq_hz = sdmmc_host_get_bus_freq(clock);
    uint32_t host_div, card_div;

    if (freq_hz >= 25000000) {
        /* 50 MHz is not reachable from a 40 MHz source; 20 MHz is the closest
         * legal rate and is what sdmmc_host asks for as "25 MHz". */
        host_div = 2;
        card_div = 0;
    } else {
        /* 400 kHz: 40 MHz / 10 / (2 * 5) is exact. */
        host_div = 10;
        card_div = 5;
    }

    /* div_factor_h at bits 12:9, div_factor_l at 16:13, div_factor_n at 20:17,
     * clk_sel at bit 23 (0 = XTAL). phase_dout = 1 gives the card clock a 90 degree
     * delay, which is what the SD spec wants for the data hold time in SDR12/SDR25;
     * the command register has to set USE_HOLD for it to apply, which we always do.
     * phase_din and phase_core stay at 0.
     */
    wr(SDMMC_CLK, 1u | ((host_div / 2 - 1) << 9) | ((host_div - 1) << 13)
                  | ((host_div - 1) << 17));

    wr(SDMMC_CLKSRC, 0);              /* slot 0 uses divider div0 */
    wr(SDMMC_CLKDIV, card_div);
    wr(SDMMC_CLKENA, 1u | (1u << 16)); /* clock enabled, low-power gating on */

    /* The timeout is counted in card output clocks, so it has to be derived from
     * the clock we actually generated, not from the rate sdmmc_host asked for:
     * the "25 MHz" slot is really 20 MHz here. ESP-IDF uses 100 ms.
     */
    wr(SDMMC_TMOUT, 255u | ((XT_XTAL_FREQ / (host_div * (card_div ? 2u * card_div : 1u)) / 10u) << 8));
}

static void arm_descriptors(struct sdmmc_ctl *ctl, void *buf, uint32_t bytes)
{
    int n = (int)((bytes + DESC_BUF_BYTES - 1) / DESC_BUF_BYTES);

    /* No need to clear the descriptors beyond n: the ring is closed back onto
     * desc[0], so the engine never walks past the last one we arm. */
    for (int i = 0; i < n; i++) {
        uint32_t chunk = bytes - (uint32_t)i * DESC_BUF_BYTES;
        if (chunk > DESC_BUF_BYTES)
            chunk = DESC_BUF_BYTES;

        /* The IDMAC rounds the size up to a whole word; the ring is closed so the
         * engine suspends on the first descriptor it has already retired rather
         * than walking off the end of the array.
         *
         * end_of_ring is deliberately not set. ESP-IDF never sets it either: the
         * chain is walked through DES3, and the engine stops when it reaches a
         * descriptor whose owner bit is still 0.
         */
        ctl->desc[i].flags = DESC_OWNER | DESC_CHAIN
                             | (i == 0 ? DESC_FIRST : 0)
                             | (i == n - 1 ? DESC_LAST : 0);
        ctl->desc[i].size = (chunk + 3) & ~3u;
        ctl->desc[i].buffer = (uint32_t)(uintptr_t)buf + (uint32_t)i * DESC_BUF_BYTES;
        ctl->desc[i].next = (uint32_t)(uintptr_t)&ctl->desc[(i + 1) % n];
    }

    /* The DMA reads the descriptors straight out of memory, so the ring has to be
     * committed before the engine is handed it. */
    commit_dcache_range(ctl->desc, n * sizeof(struct sdmmc_desc));
}

static bool dma_finished(const struct sdmmc_ctl *ctl, int n)
{
    /* The owner bits are written by the DMA, not by the CPU, so the cached copy
     * has to be dropped before the CPU looks at them. */
    discard_dcache_range(ctl->desc, n * sizeof(struct sdmmc_desc));

    for (int i = 0; i < n; i++)
        if (ctl->desc[i].flags & DESC_OWNER)
            return false;

    return true;
}

static int sdmmc_irq_trace;

void ICODE_ATTR sdmmc_irq(void)
{
    if (sdmmc_irq_trace < 8) {
        sdmmc_irq_trace++;
    }
    uint32_t pending = rd(SDMMC_MINTSTS) & (CMD_ERR_MASK | DATA_ERR_MASK | INT_CMD_DONE);
    wr(SDMMC_RINTSTS, pending);

    uint32_t dma = rd(SDMMC_IDSTS) & (DMA_DONE_MASK | DMA_ERR_MASK);
    wr(SDMMC_IDSTS, dma);

    /* RINTSTS is cleared by the write, so MINTSTS goes to zero with it. The events
     * have to be latched here or the waiting thread would see nothing. */
    sdmmc_ctl.events |= pending;
    sdmmc_ctl.dma_events |= dma;

    if (pending || dma)
        semaphore_release(&sdmmc_sem);
}

static int sdmmc_submit_command(void *controller,
                                const struct sdmmc_host_command *cmd,
                                struct sdmmc_host_response *resp)
{
    struct sdmmc_ctl *ctl = controller;

    uint32_t cmdreg = 0;
    uint32_t events;
    uint32_t bytes = 0;
    int ndesc = 0;
    int rc = SDMMC_STATUS_OK;

    /* CMD0 is the only command that sends the 80-clock initialisation sequence,
     * and it must not wait for a previous transfer. Everything else waits. */
    if (cmd->command == SD_GO_IDLE_STATE)
        cmdreg |= CMD_SEND_INIT;
    else if (cmd->command == SD_STOP_TRANSMISSION)
        cmdreg |= CMD_STOP_ABORT;
    else
        cmdreg |= CMD_WAIT_PRVDATA;

    switch (SDMMC_RESP_LENGTH(cmd->flags)) {
    case SDMMC_RESP_NONE:
        ctl->resp_len = 0;
        break;
    case SDMMC_RESP_SHORT:
        cmdreg |= CMD_RESP_EXPECT;
        if (!(cmd->flags & SDMMC_RESP_NOCRC))
            cmdreg |= CMD_CHECK_CRC;
        ctl->resp_len = 1;
        break;
    case SDMMC_RESP_LONG:
        cmdreg |= CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_CHECK_CRC;
        ctl->resp_len = 4;
        break;
    default:
        return SDMMC_STATUS_ERROR;
    }

    cmdreg |= CMD_USE_HOLD;   /* card_num is 0, use the delayed clock for hold time */

    events = CMD_ERR_MASK | INT_CMD_DONE;

    if (SDMMC_DATA_PRESENT(cmd->flags)) {
        bytes = (uint32_t)cmd->nr_blocks * cmd->block_len;
        ndesc = (int)((bytes + DESC_BUF_BYTES - 1) / DESC_BUF_BYTES);

        if (ndesc > DESC_COUNT || ((uintptr_t)cmd->buffer & 3))
            return SDMMC_STATUS_ERROR;

        cmdreg |= CMD_DATA_EXPECTED;
        if (SDMMC_DATA_DIR(cmd->flags) == SDMMC_DATA_WRITE)
            commit_dcache_range(cmd->buffer, bytes);
        else
            discard_dcache_range(cmd->buffer, bytes);

        arm_descriptors(ctl, cmd->buffer, bytes);

        wr(SDMMC_BYTCNT, bytes);
        wr(SDMMC_BLKSIZ, cmd->block_len);
        wr(SDMMC_DBADDR, (uint32_t)(uintptr_t)ctl->desc);
        wr(SDMMC_BMOD, BMOD_ENABLE | BMOD_FIXED_BURST);
        wr(SDMMC_IDINTEN, DMA_TI | DMA_RI | DMA_NI);
        wr(SDMMC_CTRL, rd(SDMMC_CTRL) | CTRL_DMA_ENABLE | CTRL_USE_IDMAC);

        events |= DATA_ERR_MASK | INT_DATA_OVER;
    }

    ctl->events = 0;
    ctl->dma_events = 0;

    wr(SDMMC_RINTSTS, 0xffffffff);
    wr(SDMMC_IDSTS, 0xffffffff);
    wr(SDMMC_INTMASK, events);       /* a set bit enables that interrupt */
    wr(SDMMC_CTRL, rd(SDMMC_CTRL) | CTRL_INT_ENABLE);

    wr(SDMMC_CMDARG, cmd->argument);
    wr(SDMMC_CMD, cmdreg | (cmd->command << CMD_INDEX_SHIFT) | CMD_START);

    /* The FSM suspends when it reaches a descriptor the host has not armed yet. */
    if (SDMMC_DATA_PRESENT(cmd->flags))
        wr(SDMMC_PLDMND, 1);


    /* A command with no data is finished when the controller reports it done. A
     * transfer is finished when the data has both left the card and reached
     * memory. The controller's own response/data timeouts bound this, but the
     * loop is bounded too so a wedged peripheral cannot hang the thread. */
    bool done = false;
    for (int tick = 0; tick < 2 * HZ; tick++) {

        /* Poll the controller directly. The events are latched in RINTSTS/IDSTS
         * and cleared by writing them back, so a poll sees exactly what the
         * interrupt would have reported. */
        uint32_t status = rd(SDMMC_RINTSTS) & (CMD_ERR_MASK | INT_CMD_DONE |
                                              INT_DATA_OVER);
        uint32_t dma = rd(SDMMC_IDSTS) & (DMA_DONE_MASK | DMA_ERR_MASK);

        if (status)
            wr(SDMMC_RINTSTS, status);
        if (dma)
            wr(SDMMC_IDSTS, dma);

        if (status || dma) {
            ctl->events |= status;
            ctl->dma_events |= dma;

            if (status & CMD_ERR_MASK) {
                rc = (status & INT_RTO) ? SDMMC_STATUS_TIMEOUT
                                        : SDMMC_STATUS_ERROR;
                done = true;
                break;
            }

            if (SDMMC_DATA_PRESENT(cmd->flags)) {
                if (dma & DMA_ERR_MASK) {
                    rc = SDMMC_STATUS_ERROR;
                    done = true;
                    break;
                }
                if ((status & INT_DATA_OVER) && dma_finished(ctl, ndesc)) {
                    done = true;
                    break;
                }
                udelay(250);
                continue;
            }

            if (status & INT_CMD_DONE) {
                if (cmd->flags & SDMMC_RESP_BUSY) {
                    int waited = 0;
                    while ((rd(SDMMC_STATUS) & STATUS_DATA_BUSY) && waited < 1000) {
                        udelay(100);
                        waited++;
                    }
                    if (waited >= 1000)
                        rc = SDMMC_STATUS_TIMEOUT;
                }
                done = true;
                break;
            }
        }
        udelay(250);
    }

    if (SDMMC_DATA_PRESENT(cmd->flags)) {
        wr(SDMMC_CTRL, rd(SDMMC_CTRL) & ~(CTRL_DMA_ENABLE | CTRL_USE_IDMAC));
        wr(SDMMC_BMOD, 0);

        if (rc == SDMMC_STATUS_OK && SDMMC_DATA_DIR(cmd->flags) == SDMMC_DATA_READ)
            discard_dcache_range(cmd->buffer, bytes);
    }

    return rc;
}

static void sdmmc_set_power_enabled(void *controller, bool enable)
{
    struct sdmmc_ctl *ctl = controller;
    int waited;

    if (enable) {
        wr(XT_SYSTEM_BASE + 0x1c, rd(XT_SYSTEM_BASE + 0x1c) | (1u << 7));
        wr(SDMMC_PWREN, 1);

        wr(SDMMC_CTRL, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        /* The three reset bits are self-clearing on real hardware; the bound is
         * there so a wedged peripheral cannot hang the boot. */
        waited = 0;
        while (rd(SDMMC_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) {
            if (++waited > 1000)
                break;
            udelay(10);
        }

        wr(SDMMC_INTMASK, 0);
        wr(SDMMC_RINTSTS, 0xffffffff);
        wr(SDMMC_IDSTS, 0xffffffff);
        wr(SDMMC_IDINTEN, 0);
        wr(SDMMC_CTRL, 0);
        wr(SDMMC_BMOD, BMOD_SW_RESET);

        set_clock(ctl->bus_clock);

        /* The card is powered by the board, not by this controller; PWREN is left
         * alone and "power on" means the controller is reset and clocked. */
        wr(SDMMC_RST_N, 3u);         /* active low: 1 = not reset */
        ctl->powered = true;
    } else {
        wr(SDMMC_CLKENA, 0);
        wr(SDMMC_PWREN, 0);
        wr(SDMMC_CTRL, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        waited = 0;
        while (rd(SDMMC_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) {
            if (++waited > 1000)
                break;
            udelay(10);
        }
        wr(XT_SYSTEM_BASE + 0x1c, rd(XT_SYSTEM_BASE + 0x1c) & ~(1u << 7));
        ctl->powered = false;
    }
}

static void sdmmc_set_bus_width(void *controller, uint32_t width)
{
    (void)controller;

    /* CTYPE has one bit per card: 0 = 1-bit, 1 = 4-bit. Only D0 is wired on this
     * board, so 4-bit is never requested. */
    if (width == SDMMC_BUS_WIDTH_4BIT)
        wr(SDMMC_CTYPE, rd(SDMMC_CTYPE) | 1u);
    else
        wr(SDMMC_CTYPE, rd(SDMMC_CTYPE) & ~1u);
}

static void sdmmc_set_bus_clock(void *controller, uint32_t clock)
{
    struct sdmmc_ctl *ctl = controller;

    ctl->bus_clock = clock;

    if (ctl->powered)
        set_clock(clock);
}

static const struct sdmmc_controller_ops sdmmc_ops = {
    .set_power_enabled = sdmmc_set_power_enabled,
    .set_bus_width     = sdmmc_set_bus_width,
    .set_bus_clock     = sdmmc_set_bus_clock,
    .submit_command    = sdmmc_submit_command,
};

static const struct sdmmc_host_config sdmmc_config INITDATA_ATTR = {
    .type = STORAGE_SD,
    .bus_voltages = SDMMC_BUS_VOLTAGE_3V2_3V3 |
                    SDMMC_BUS_VOLTAGE_3V3_3V4,
    /* Only D0 is connected, so the bus is one bit wide. */
    .bus_widths = SDMMC_BUS_WIDTH_1BIT,
    /* 400 kHz and 25 MHz are the two rates sdmmc_host requires; 50 MHz cannot be
     * generated from the 40 MHz crystal. */
    .bus_clocks = SDMMC_BUS_CLOCK_400KHZ |
                  SDMMC_BUS_CLOCK_25MHZ,
    .max_nr_blocks = MAX_NR_BLOCKS,
    .is_removable = false,
};

void sdmmc_host_target_init(void)
{
    /* The SDHOST clock gate is off at reset; ESP-IDF's sdmmc_ll_enable_bus_clock
     * sets bit 7 of SYSTEM_PERIP_CLK_EN1 and nothing else is needed to reach the
     * block. The reset bit is pulsed so the controller starts from its documented
     * default state rather than whatever a previous image left behind.
     */
    wr(XT_SYSTEM_BASE + 0x1C, rd(XT_SYSTEM_BASE + 0x1C) | (1u << 7));
    wr(XT_SYSTEM_BASE + 0x20, 1u << 7);
    udelay(10);
    wr(XT_SYSTEM_BASE + 0x20, 0u);
    udelay(10);

    /* Route the three lines through the GPIO matrix. CLK is driven by the
     * controller; CMD and D0 are bidirectional, so both halves have to be
     * connected. */
    io_mux_select_gpio(47);
    io_mux_select_gpio(48);
    io_mux_select_gpio(21);

    gpio_matrix_out(47, SIG_SDHOST_CLK);
    gpio_matrix_out(48, SIG_SDHOST_CMD);
    gpio_matrix_in(48, SIG_SDHOST_CMD);
    gpio_matrix_out(21, SIG_SDHOST_D0);
    gpio_matrix_in(21, SIG_SDHOST_D0);

    gpio_set_direction(47, 1);
    gpio_set_direction(48, 1);
    gpio_set_direction(21, 1);

    wr(XT_INTERRUPT_CORE0_BASE + 4 * XT_SRC_SDIO_HOST, XT_SLOT_SDHOST);
    semaphore_init(&sdmmc_sem, 1, 0);

    sdmmc_host_init(&sdmmc_host, &sdmmc_config, &sdmmc_ops, &sdmmc_ctl);
}
