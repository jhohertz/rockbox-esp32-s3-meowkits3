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
/* The value the display was confirmed working with, recorded in e2a8c24dd4 when
 * the RGBW quadrant test pattern rendered correctly on this glass: bit 31
 * CLK_EQU_SYSCLK, taking the source clock undivided -- 80 MHz from the PLL that
 * CLK_GATE bit 2 selects. Not a divided clock. Both the reference firmware and our own bring-up measurement
 * drive the panel this way, and every divided configuration tried since has
 * produced banding and noise over a solid fill. */
#define SPI_CLK_80M     SPI_CLK_EQU_SYSCLK

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
    /* The one register IDF must not keep. spi_bus_add_device() programmed a
     * legal 20 MHz divider, and it is the last thing standing between us and the
     * configuration that is known to paint: e2a8c24dd4 confirmed the quadrant
     * test pattern with clk=0x80000000, source undivided. Everything else in
     * that record -- user 0x18000001, ctrl 0, gate 7 -- is now matched. */
    wr(SPI_CLOCK, SPI_CLK_80M);
    spi_apply_config();

    wr(SPI_MS_DLEN, SPI_BUF_BYTES * 32 - 1);
    spi_apply_config();
}

/* Spin until the controller has finished clocking. The reference's
 * dc_control() does exactly this before it touches the DC pin: if DC changes
 * while a transfer is still shifting out, the panel samples the remaining bytes
 * under the wrong register select and the command/data stream desynchronises.
 * On the glass that is banding and noise over a solid fill, not a geometry
 * error -- which is why it is worth the wait. */
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

        while (rd(SPI_CMD) & SPI_CMD_USR)
        {
            if (++spins > 200000)
            {
                break;
            }
        }
        len -= n;
        data += n;
    }
}
