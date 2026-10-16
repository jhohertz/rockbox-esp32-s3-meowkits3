/***************************************************************************
 * GPSPI2 master, polled, for the ST7789 panel.
 *
 * Three things are easy to get wrong on this peripheral:
 *
 * 1. The clock does not come from the 240 MHz CPU clock. The master clock
 *    source is selected in CLK_GATE: bit 2 chooses PLL_CLK_80M, cleared gives
 *    XTAL (40 MHz). CLK_EQU_SYSCLK in the CLOCK register makes spi_clk equal
 *    that source directly, which is the fastest setting available - and the
 *    reference firmware drives this panel at 80 MHz.
 *
 * 2. GPSPI has no "general read/write" mode bit. DMA mode is off unless
 *    dma_tx_ena/dma_rx_ena are set, so the CPU-controlled 16-word buffer at
 *    0x98 is the normal path.
 *
 * 3. The buffer is a little-endian byte array: the first byte to go out sits
 *    in bits 0-7 of SPI_W0. Both ESP-IDF's spi_ll_write_buffer_byte() and the
 *    Arduino ESP32 SPI driver place byte 0 in the low byte.
 *
 * The CONF-state registers (USER, CLOCK, CTRL, MS_DLEN) live in the APB domain
 * and have to be latched into the SPI clock domain with CMD.UPDATE before
 * CMD.USR starts the transfer.
 *
 * The pins are not enough on their own. GPIO 40 and 41 are MTDO and MTDI in
 * the IO MUX, and GPIO 39 is MTCK; all three default to MCU_SEL = 0, the JTAG
 * function. Routing FSPICLK/FSPIQ to "GPIO 41"/"GPIO 40" in the matrix does
 * nothing while the pad is muxed elsewhere - the transfer completes and the
 * clock line never moves. gpio_set_direction() now selects the GPIO function
 * as ESP-IDF's gpio_output_enable() does.
 ****************************************************************************/
#include "config.h"
#include "xtensa.h"
#include "spi-xtensa.h"
#include "gpio-xtensa.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "system.h"   /* mdelay, TEMPORARY probe */
#include <stdio.h>    /* TEMPORARY probe logging */

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)reg = v; }

static void spi_apply_config(void)
{
    wr(SPI_CMD, rd(SPI_CMD) | SPI_CMD_UPDATE);

    for (int i = 0; i < 2000 && (rd(SPI_CMD) & SPI_CMD_UPDATE); i++)
        ;
}

/* GPSPI clock. The module clock is PLL_CLK_80M (CLK_GATE bit 2) and the SPI
 * clock is that divided by (clkdiv_pre+1) * (clkcnt_n+1), so 80/(2*2) = 20 MHz.
 * The panel is specced to 80 MHz and the reference firmware drives it there, but
 * that path uses DMA; the polled CPU-buffer path should be proven at a slower
 * rate before trusting the top of the range.
 */
/* func_out_sel_cfg value meaning "drive this pad from GPIO_OUT_REG". */
#ifndef SIG_GPIO_OUT_IDX
#define SIG_GPIO_OUT_IDX 256
#endif

/* SPI_CLOCK on ESP32-S3 is CLKCNT_N[17:12] | CLKCNT_H[11:6] | CLKCNT_L[5:0],
 * with CLK_EQU_SYSCLK at bit 31. There is no pre-divider field. The macros this
 * file used to carry wrote (1u << 18) and (24u << 18), which are above N and
 * therefore land in reserved bits and do nothing -- so the intended
 * "80/(pre+1)/(n+1)" division never happened, and worse, SPI_CLK_20M decoded to
 * N=1 H=0 L=1. The hardware requires N >= H >= L >= 1; H < L yields a malformed
 * clock waveform rather than a slow one, which on the glass is banding and noise
 * over a solid fill, not a geometry error.
 *
 * Period is N+1 source cycles, the high phase H+1 and the low phase L+1, so
 * 80 MHz / 4 = 20 MHz is N=3, H=1, L=1.
 */
#define SPI_CLK_20M     ((3u << 12) | (1u << 6) | (1u << 0))
/* Slowest legal divider: 80 / 64 = 1.25 MHz, N=63 H=32 L=1. 200 kHz is not
 * reachable on this block because there is no pre-divider. */
#define SPI_CLK_1M25    ((63u << 12) | (32u << 6) | (1u << 0))



/* Measure the bit stream at the pin. A 64-byte transfer must produce exactly 512
 * clock edges; anything else means MS_DLEN is not being honoured and the panel is
 * receiving a stream that is longer or shorter than the buffer we filled.
 * The clock is dropped to 1 MHz so a software loop can resolve individual edges.
 */
/* The bus, brought up the way the two implementations that actually paint this
 * panel do it. Both the reference's LovyanGFX bus and the bare-metal driver
 * "use spi_bus_initialize only for pin and clock routing and then write
 * SPI_USER / SPI_MS_DLEN / SPI_CMD themselves".
 *
 * That first half is not a convenience. Measured before this call was added:
 * bit 6 of PERIP_CLK_EN0 was set, the base address was right, and yet every
 * GPSPI2 register read back 0x00000000 and a write of 0x5a5a5a5a to SPI_ADDR
 * was discarded outright. A register file that acknowledges on the APB and
 * captures nothing is a block whose module clock has never been routed, and
 * routing it is what spi_bus_initialize() does. Setting the SoC gate bit by
 * hand is necessary but not sufficient.
 *
 * So the transfers below stay hand-programmed -- that is the shape this panel
 * needs, and IDF's own master was measured emitting 0 edges against a 20/20
 * self-test -- but they now run on a block that has a clock. */
static spi_device_handle_t spi_dev;

void spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .spics_io_num = -1,   /* CS lives on the PCA9557, not a GPIO */
        .queue_size = 7,
    };
    static int initialised;

    if (!initialised)
    {
        /* SPI_DMA_DISABLED, deliberately. We need spi_bus_initialize() for the
         * clock and pin routing and nothing else, and AUTO allocates a GDMA
         * channel plus the burst-read configuration. LovyanGFX's Bus_SPI.cpp:561
         * records that a DMA burst read at 1:1 with the APB clock corrupts the
         * tail of any transfer whose length is not a multiple of 4 -- and our
         * command and argument transfers are 1, 2, 5 and 14 bytes, all of which
         * have a remainder. We drive the CPU buffer, not the DMA, so there is
         * nothing for that erratum to corrupt, but there is also no reason to
         * keep the variable in the experiment. */
        esp_err_t e = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
            printf("spi_init: spi_bus_initialize: %d\n", (int)e);
        e = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_dev);
        if (e != ESP_OK)
            printf("spi_init: spi_bus_add_device: %d\n", (int)e);
        initialised = 1;
    }

    wr(XT_SYSTEM_BASE + 0x18, rd(XT_SYSTEM_BASE + 0x18) | SYSTEM_SPI2_CLK_EN);
    wr(SPI_CLK_GATE, SPI_CLK_EN | SPI_MST_CLK_ACTIVE | SPI_MST_CLK_SEL);

    /* GPSPI does not drive its own pins; the signals have to be routed through
     * the GPIO matrix. DC is a plain GPIO, not a controller signal. */
    gpio_matrix_out(PIN_LCD_SCLK, SIG_FSPICLK_OUT);
    gpio_matrix_out(PIN_LCD_MOSI, SIG_FSPID_OUT);

    gpio_set_direction(PIN_LCD_SCLK, 1);
    gpio_set_direction(PIN_LCD_MOSI, 1);
    gpio_set_direction(PIN_LCD_DC, 1);

    /* DC needs an explicit matrix entry, and the reason is bit 10 of
     * func_out_sel_cfg, not the signal index. Left at its reset value of 0x100
     * the pad is pointed at the GPIO output register (func_sel 256) but
     * oen_sel is CLEAR, so the pad's output enable comes from the signal's own
     * oen line rather than from GPIO_ENABLE -- and for SIG_GPIO_OUT_IDX that
     * line is not driven. Measured on this board: DC read func=256 oen=0 while
     * SCLK and MOSI, both routed through gpio_matrix_out(), read oen=1. The
     * result is a tri-stated DC whose GPIO_ENABLE bit and GPIO_OUT1 writes are
     * both perfectly correct and both ignored.
     *
     * A floating DC means the panel samples every byte as a command and never
     * as data: the init sequence still takes effect, DISPON still lights the
     * panel, and RAMWR content is discarded -- backlight on, no pixels, with
     * the SPI controller reporting 2405 clean completed transfers.
     *
     * gpio_matrix_out() sets oen_from_enable, which is what the other two pads
     * get; DC was the only one left at reset. */
    gpio_matrix_out(PIN_LCD_DC, SIG_GPIO_OUT_IDX);

    wr(SPI_DMA_CONF, 0);

    /* Write-only master transfer. WR_BIT_ORDER = 0, RD_BIT_ORDER = 0. */
    wr(SPI_CTRL, 0);
    wr(SPI_USER, SPI_USR_MOSI | SPI_USR_MISO | SPI_USR_DOUTDIN);
    wr(SPI_USER1, 0);

    /* spi_clk = the 80 MHz source, divided. 80/(2*2) = 20 MHz. The reference
     * drives this panel at 80 MHz, but that path uses DMA; the polled CPU-buffer
     * path is proven at 20 MHz first.
     */
    /* Leave SPI_CLOCK to IDF. spi_bus_add_device() already programmed a legal
     * divider for the 20 MHz in its device config, and every hand-written value
     * this file has used for it has been malformed. Writing our own here only
     * clobbers the one setting IDF got right. */

    wr(SPI_MS_DLEN, SPI_BUF_BYTES * 32 - 1);
    spi_apply_config();
}

volatile unsigned int xt_spi_calls;
volatile unsigned int xt_spi_chunks;
volatile unsigned int xt_spi_stuck;
volatile unsigned int xt_spi_spins;
volatile unsigned int xt_spi_maxlen;
volatile unsigned int xt_spi_iter_max;

/* Edge probe (see spi_edge_probe_run at the end of this file). Declared here
   because spi_write's wait loop is the thing doing the counting. */
#define SPI_PROBE_SCLK_BIT (1u << (PIN_LCD_SCLK - 32))
#define SPI_PROBE_MOSI_BIT (1u << (PIN_LCD_MOSI - 32))

volatile unsigned xt_spi_edges;
volatile unsigned xt_spi_mosi_chg;
volatile int spi_edge_probe;

/* Spin until the controller has finished clocking. The reference's
 * dc_control() does exactly this before it touches the DC pin: if DC changes
 * while a transfer is still shifting out, the panel samples the remaining
 * bytes under the wrong register-select and the command/data stream
 * desynchronises -- which on the glass is banding and noise over a solid fill,
 * not a clean geometry error. */
void spi_wait_idle(void)
{
    for (int i = 0; i < 200000; i++)
        if (!(rd(SPI_CMD) & SPI_CMD_USR))
            return;
}

void spi_write(const unsigned char *data, int len)
{
    while (len > 0)
    {
        int words = (len + 3) / 4;
        if (words > SPI_BUF_WORDS)
            words = SPI_BUF_WORDS;

        /* The FIFO is addressed in whole words, but the transfer length is not:
         * MS_DLEN is an exact bit count. Rounding it up to a multiple of 32 pads
         * every command with 24 zero bits, which the panel reads as argument
         * bytes, so CASET/RASET never receive the window they were given.
         */
        int n = len < words * 4 ? len : words * 4;

        for (int i = 0; i < words; i++)
        {
            uint32_t w = 0;

            for (int b = 0; b < 4; b++)
            {
                int idx = i * 4 + b;
                if (idx < n)
                    w |= (uint32_t)data[idx] << (8 * b);
            }

            wr(SPI_W0 + 4 * (unsigned)i, w);
        }

        wr(SPI_MS_DLEN, (unsigned)n * 8u - 1u);
        spi_apply_config();
        /* Plain write, not read-modify-write: the reference does
         * `*_spi_cmd_reg = SPI_EXECUTE`. The UPDATE bit latched above is still
         * self-clearing, and OR-ing a stale read back in re-triggers it. */
        wr(SPI_CMD, SPI_CMD_USR);

        /* USR is self-clearing: it drops when the transfer completes. Bounded so
           a stalled transfer is countable rather than a silent hang -- an
           unbounded spin here is indistinguishable from never having arrived. */
        unsigned spins = 0;
        unsigned last_s = 0, last_m = 0;

        if (spi_edge_probe)
        {
            unsigned in1 = rd(GPIO_IN1);
            last_s = (in1 & SPI_PROBE_SCLK_BIT) ? 1u : 0u;
            last_m = (in1 & SPI_PROBE_MOSI_BIT) ? 1u : 0u;
        }

        while (rd(SPI_CMD) & SPI_CMD_USR)
        {
            if (spi_edge_probe)
            {
                unsigned in1 = rd(GPIO_IN1);
                unsigned s = (in1 & SPI_PROBE_SCLK_BIT) ? 1u : 0u;
                unsigned m = (in1 & SPI_PROBE_MOSI_BIT) ? 1u : 0u;

                if (s != last_s) { xt_spi_edges++; last_s = s; }
                if (m != last_m) { xt_spi_mosi_chg++; last_m = m; }
            }

            if (++spins > 200000)
            {
                xt_spi_stuck++;
                break;
            }
        }
        /* How long USR stayed set. This is the difference between a controller
         * that clocked out the buffer and one that cleared USR immediately
         * because it had nothing to send: at 200 kHz a 512-bit transfer is
         * 2.56 ms, which is thousands of these iterations, while a transfer the
         * controller ignored returns in a couple. */
        if (spins > xt_spi_iter_max)
            xt_spi_iter_max = spins;
        xt_spi_spins += spins;
        xt_spi_calls++;

        len -= n;
        data += n;
    }
}


/* ------------------------------------------------------------------ */
/* TEMPORARY: edge-counting probe, ported from idf-spike stage 7.      */
/* ------------------------------------------------------------------ */

/* Everything upstream of this point reports success: the PCA9557 holds CS
 * asserted and RST released and confirms it on its input register, the pads
 * read the right func_sel and oen_sel, and 2405 transfers complete with none
 * stuck. All of that is the register we hoped would produce a waveform. This
 * measures the waveform.
 *
 * The pads are already in INPUT_OUTPUT (gpio_set_direction sets FUN_IE), so
 * GPIO_IN1 mirrors the pin while the controller drives it. Counting there is
 * the first check in this whole path that looks at the wire rather than at the
 * programming of the block attached to it.
 *
 * A zero only means something if the counter works, so the probe validates
 * itself afterwards by reconnecting SCLK to the plain GPIO output signal and
 * toggling it by hand; the same loop must count twenty edges. */
void spi_edge_probe_run(void)
{
    static unsigned char pat[64];
    unsigned clock_save = rd(SPI_CLOCK);
    unsigned last_s;
    int i, vedges = 0;

    for (i = 0; i < 64; i++)
        pat[i] = 0xA5;

    wr(SPI_CLOCK, SPI_CLK_1M25);
    spi_apply_config();

    xt_spi_edges = 0;
    xt_spi_mosi_chg = 0;
    xt_spi_spins = 0;
    xt_spi_iter_max = 0;
    spi_edge_probe = 1;
    spi_write(pat, sizeof(pat));
    spi_edge_probe = 0;

    printf("spi probe: 512 bits @1.25MHz -> sclk edges=%u (want >=512), "
           "mosi chg=%u, stuck=%u\n",
           xt_spi_edges, xt_spi_mosi_chg, xt_spi_stuck);
    printf("spi probe: spins max=%u total=%u (0.41ms at 1.25MHz is hundreds; ~0 means "
           "the controller clocked nothing)\n", xt_spi_iter_max, xt_spi_spins);
    printf("spi probe: clock=0x%08x ms_dlen=%u user=0x%08x user1=0x%08x "
           "ctrl=0x%08x clk_gate=0x%08x cmd=0x%08x\n",
           (unsigned)rd(SPI_CLOCK), (unsigned)rd(SPI_MS_DLEN),
           (unsigned)rd(SPI_USER), (unsigned)rd(SPI_USER1),
           (unsigned)rd(SPI_CTRL), (unsigned)rd(SPI_CLK_GATE),
           (unsigned)rd(SPI_CMD));

    /* Validate the counter. If this does not read 20 then the readback path is
     * the problem and the number above means nothing. */
    gpio_matrix_out(PIN_LCD_SCLK, SIG_GPIO_OUT_IDX);
    last_s = (rd(GPIO_IN1) & SPI_PROBE_SCLK_BIT) ? 1u : 0u;
    for (i = 0; i < 20; i++)
    {
        wr((i & 1) ? GPIO_OUT1_W1TC : GPIO_OUT1_W1TS, SPI_PROBE_SCLK_BIT);
        mdelay(1);
        unsigned now = (rd(GPIO_IN1) & SPI_PROBE_SCLK_BIT) ? 1u : 0u;
        if (now != last_s) { vedges++; last_s = now; }
    }
    printf("spi probe self-test: 20 hand toggles -> %d edges (want 20)\n",
           vedges);

    /* Is the block writable at all? SPI_ADDR is a plain 32-bit register with no
     * side effects, so it is the safe place to ask. If the magic value does not
     * come back, the peripheral has no clock or is held in reset, and every
     * "transfer completed" on this driver has been a no-op against a dead
     * register file. Also read the system gate itself rather than trusting that
     * the write at init landed: bit 6 of PERIP_CLK_EN0, whose reset default is
     * already 1. */
    wr(SPI_ADDR, 0x5a5a5a5au);
    printf("spi probe: addr write 0x5a5a5a5a -> read 0x%08x  "
           "perip_clk_en0=0x%08x (bit6=%d) en1=0x%08x\n",
           (unsigned)rd(SPI_ADDR), (unsigned)rd(XT_SYSTEM_BASE + 0x18),
           (int)((rd(XT_SYSTEM_BASE + 0x18) >> 6) & 1u),
           (unsigned)rd(XT_SYSTEM_BASE + 0x1c));

    gpio_matrix_out(PIN_LCD_SCLK, SIG_FSPICLK_OUT);
    wr(SPI_CLOCK, clock_save);
    spi_apply_config();
}
