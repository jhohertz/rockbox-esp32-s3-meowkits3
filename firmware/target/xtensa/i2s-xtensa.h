/***************************************************************************
 * ESP32-S3 I2S1 (TX) and GDMA register access.
 *
 * Register bases and bit positions taken from
 * components/soc/esp32s3/register/soc/{reg_base,i2s_reg,gdma_reg,system_reg}.h,
 * not guessed.
 ****************************************************************************/
#ifndef I2S_XTENSA_H
#define I2S_XTENSA_H

#include <stdint.h>

#define I2S0_BASE   0x6000F000
#define I2S1_BASE   0x6002D000
#define GDMA_BASE   0x6003F000
#define SYSTEM_BASE 0x600C0000

/* Peripheral clock gating. Both bits default to 0, so the bootloader does not
 * enable them: Rockbox has to. */
#define SYSTEM_PERIP_CLK_EN0 (SYSTEM_BASE + 0x18)
#define SYSTEM_PERIP_CLK_EN1 (SYSTEM_BASE + 0x1C)
/* Assert-high reset, one bit per peripheral at the same position as its clock
 * enable. Enabling the clock without clearing this leaves the peripheral held in
 * reset: its registers then accept no writes and read back as zero, which looks
 * identical to a wrong base address. ESP-IDF releases these in periph_ctrl rather
 * than leaving it to each driver. */
#define SYSTEM_PERIP_RST_EN0 (SYSTEM_BASE + 0x20)
#define SYSTEM_PERIP_RST_EN1 (SYSTEM_BASE + 0x24)
#define CLK_EN_I2S0 (1u << 4)
#define CLK_EN_I2S1 (1u << 21)
#define CLK_EN_DMA  (1u << 6)

/* I2S1, TX side only */
#define I2S_TX_CONF          (I2S1_BASE + 0x24)
#define I2S_TX_CONF1         (I2S1_BASE + 0x2C)
#define I2S_TX_CLKM_CONF     (I2S1_BASE + 0x34)
#define I2S_TX_CLKM_DIV_CONF (I2S1_BASE + 0x3C)
#define I2S_TX_TDM_CTRL      (I2S1_BASE + 0x54)

#define TX_CONF_UPDATE       (1u << 8)   /* latch APB writes into the clock domain */
#define TX_CONF_START        (1u << 2)
#define TX_CONF_FIFO_RESET   (1u << 1)
#define TX_CONF_RESET        (1u << 0)

/* TX_CLKM_CONF / RX_CLKM_CONF fields on this part:
 *   I2S_CLK_EN        [29]   module clock enable, shared by TX and RX
 *   tx_clk_sel        [28:27]
 *   tx_clk_active     [26]   starts the clock generator
 *   tx_clkm_div_num   [8:0]
 *
 * An earlier revision of this file claimed bit 29 was not a field and removed
 * it. That was wrong and the claim is recorded here so it is not repeated: the
 * field is named I2S_CLK_EN with no TX_ prefix, so a scan for I2S_TX_* does not
 * find it. Concluding that something is absent from a search that could not have
 * found it is the failure mode this port has hit repeatedly.
 *
 * clk_sel encoding is taken from i2s_ll_tx_clk_set_src() in
 * components/hal/esp32s3/include/hal/i2s_ll.h, not guessed:
 *   0 = XTAL   1 = PLL_240M   2 = PLL_160M   3 = EXTERNAL
 * An earlier comment here called 1 "APLL", which this part does not offer.
 *
 * PLL_F160M is what ESP-IDF picks for I2S_CLK_SRC_DEFAULT. Note that switching
 * to it changed nothing measurable on the stall, so the clock source was not the
 * fault; it is kept because it is IDF's supported audio path and removes the
 * fractional modulator from 40 MHz from the list of suspects.
 */
#define CLKM_CONF_CLK_ACTIVE (1u << 26)
/* I2S_CLK_EN -- note the name carries no TX/RX prefix, which is exactly how it
 * got missed: a scan for I2S_TX_* fields does not find it, and it was briefly
 * removed on the mistaken grounds that bit 29 was undefined. It is defined
 * (I2S_CLK_EN_M = BIT(29) in i2s_reg.h) and it is the module clock enable for
 * this register, distinct from tx_clk_active which starts the generator. */
#define CLKM_CONF_CLK_EN     (1u << 29)
#define CLKM_CONF_CLK_SEL_PLL160M 2

/* I2S_STATE_REG(i) = REG_I2S_BASE(i) + 0x6C; tx_idle is bit 0. This is the
 * transmitter's own statement of whether its state machine is running, as
 * opposed to GDMA's view of the same transfer. */
#define I2S_TX_STATE      (I2S1_BASE + 0x6C)
#define TX_STATE_IDLE     (1u << 0)

/* I2S0, RX side. The receiver runs in slave mode: it takes BCLK and WS from the
 * GPIO matrix instead of generating them, so the ADC is locked to the frame the
 * transmitter is already driving out of pins 13/14.
 */
#define I2S0_RX_CONF          (I2S0_BASE + 0x20)
#define I2S0_RX_CONF1         (I2S0_BASE + 0x28)
#define I2S0_RX_CLKM_CONF     (I2S0_BASE + 0x30)
#define I2S0_RX_CLKM_DIV_CONF (I2S0_BASE + 0x38)
#define I2S0_RX_TDM_CTRL      (I2S0_BASE + 0x50)

#define RX_CONF_UPDATE       (1u << 8)
#define RX_CONF_START        (1u << 2)
#define RX_CONF_FIFO_RESET   (1u << 1)
#define RX_CONF_RESET        (1u << 0)
#define RX_CONF_SLAVE        (1u << 3)   /* clock from the incoming BCLK/WS */
#define RX_CONF_LEFT_ALIGN   (1u << 15)  /* reset default, must be kept */
#define RX_CONF_STOP_MODE    (1u << 13)  /* reset default, must be kept */
#define RX_CONF_PCM_BYPASS   (1u << 12)  /* reset default, must be kept */
#define RX_CONF_TDM_EN       (1u << 19)

/* GDMA RX channel 0 */
#define GDMA_RX_CONF0   (GDMA_BASE + 0x00)
#define GDMA_RX_CONF1   (GDMA_BASE + 0x04)
#define GDMA_RX_INT_ST  (GDMA_BASE + 0x0C)
#define GDMA_RX_INT_ENA (GDMA_BASE + 0x10)
#define GDMA_RX_INT_CLR (GDMA_BASE + 0x14)
#define GDMA_RX_LINK    (GDMA_BASE + 0x20)
#define GDMA_RX_PERI_SEL (GDMA_BASE + 0x48)

/* IN_DONE (bit 0) is the last data of a descriptor landing in memory; IN_EOF (bit 1)
 * fires when the data has merely been read out of the peripheral. */
#define GDMA_INT_IN_DONE    (1u << 0)
#define GDMA_INT_IN_EOF     (1u << 1)
#define GDMA_INT_IN_DSCR_ERR (1u << 3)
#define GDMA_INT_IN_DSCR_EMPTY (1u << 4)

/* The IN link has auto_ret between addr and stop, which shifts stop and start up by
 * one relative to the OUT link. */
#define GDMA_RX_LINK_START  (1u << 22)
#define GDMA_RX_LINK_STOP   (1u << 21)

#define GDMA_TRIG_I2S0 3

/* GDMA TX channel 0 */
#define GDMA_TX_CONF0   (GDMA_BASE + 0x60)
#define GDMA_TX_CONF1   (GDMA_BASE + 0x64)
#define GDMA_TX_INT_RAW (GDMA_BASE + 0x68)
#define GDMA_TX_INT_ST  (GDMA_BASE + 0x6C)
#define GDMA_TX_INT_ENA (GDMA_BASE + 0x70)
#define GDMA_TX_INT_CLR (GDMA_BASE + 0x74)
/* out_fifo_empty_l1/l2/l3 and out_fifo_full_l1/l2/l3 in the low six bits. The
 * subordinate FIFOs are the path to the peripheral, so "all empty while the DMA
 * still owns a descriptor" distinguishes a stalled transmitter from an
 * undelivered interrupt. */
#define GDMA_TX_FIFO_STATUS (GDMA_BASE + 0x78)
/* out_state[22:19] and out_dscr_state[18:17]: the channel's own idea of what it
 * is doing. There is no separate DSCR_STATE register on this part. */
#define GDMA_TX_STATE   (GDMA_BASE + 0x84)

/* OUT channel completion events, positions taken from the BIT() definitions in
 * components/soc/esp32s3/register/soc/gdma_reg.h rather than from prose:
 *
 *   bit 0  out_reach_target   GDMA_OUT_REACH_TARGET_CH0
 *   bit 1  out_eof            GDMA_OUT_EOF_CH0_INT_*   = BIT(1)
 *   bit 2  out_dscr_err       GDMA_OUT_DSCR_ERR_CH0_INT_* = BIT(2)
 *   bit 3  out_total_eof      GDMA_OUT_TOTAL_EOF_CH0_INT_* = BIT(3)
 *
 * There is no separate "done" bit on the OUT channel: out_eof IS the
 * per-descriptor completion event, and it is what ESP-IDF's own GDMA driver
 * treats as the transmit-complete callback. An earlier note here claimed bit 0
 * was DONE and bit 1 an earlier EOF; bit 0 is actually out_reach_target, which
 * only fires when out_eof_num is programmed, and it never is. Enabling bit 0
 * therefore meant the transmit interrupt never fired, the ring starved after two
 * descriptors, and nothing played.
 */
#define GDMA_INT_DONE (1u << 1)          /* out_eof: this descriptor is finished */
#define GDMA_INT_DSCR_ERR (1u << 2)
#define GDMA_INT_TOTAL_EOF (1u << 3)
#define GDMA_TX_LINK    (GDMA_BASE + 0x80)
#define GDMA_TX_PERI_SEL (GDMA_BASE + 0xA8)

#define GDMA_TRIG_I2S1 4

/* 16-bit, two slots: BCLK = 32 * fs. With MCLK = 256 * fs the I2S peripheral's
 * BCLK divider is 8. The codec's own BCLK divider is a different thing: it
 * divides the codec's internal clock (256 * fs) by 2 * bclk_div = 8 to give the
 * same 32 * fs. */
#define MCLK_MULTIPLE 256
#define SLOT_BITS     16
#define SLOT_NUM      2
#define I2S_BCLK_DIV  (MCLK_MULTIPLE / (SLOT_BITS * SLOT_NUM))   /* 8 */

/* Descriptor fields. Only the low 20 bits of the descriptor address are usable
 * through GDMA_TX_LINK, so descriptors must sit in the 0x3FC00000 window, which
 * is where .bss is placed. The buffer field is a full 32-bit address. */
#define GDMA_MAX_DESC_LEN 4095

/* A descriptor cannot exceed 4095 bytes, and the DMA moves in whole words, so the
 * recording chunk is the largest multiple of 4 below the limit: 1023 stereo frames.
 */
#define GDMA_RX_CHUNK 4092

/* MeowKit wiring (bsp/config.h). */
#define PIN_I2S_MCLK 15
#define PIN_I2S_BCLK 14
#define PIN_I2S_WS   13
#define PIN_I2S_DOUT 16
#define PIN_I2S_DIN  3   /* ES7210 -> I2S0 receive */

/* One GDMA link-list item.
 *
 * The hardware reads exactly the first three words. The padding to 32 bytes is
 * deliberate and is the reason this is not a plain three-word struct: two
 * 12-byte descriptors sit in a 24-byte array and would share one 32-byte
 * D-cache line, and cache maintenance works at line granularity. Committing the
 * descriptor the CPU just armed would then also write back the CPU's stale copy
 * of its neighbour -- including an owner bit the DMA had already cleared -- so
 * the engine would be handed back a descriptor it had retired and the CPU
 * believed was still in flight. One descriptor per line confines each commit to
 * the descriptor actually being handed over.
 *
 * Offsets the DMA cares about are unchanged: dw0 at 0, buffer at 4, next at 8.
 */
struct gdma_desc {
    uint32_t dw0;      /* size[11:0], length[23:12], err_eof[28], suc_eof[30], owner[31] */
    uint32_t buffer;
    uint32_t next;
    uint32_t pad[5];   /* to one esp32s3 D-cache line; see above */
} __attribute__((aligned(32)));

/* dw0: size[11:0] is the buffer capacity, length[23:12] is what the DMA moves. */
#define DESC_SUC_EOF (1u << 30)
#define DESC_OWNER   (1u << 31)

#define OWNER_CPU 0
#define OWNER_DMA 1

void i2s_init(void);

#ifdef HAVE_RECORDING
bool i2s_rx_chan_create(void);   /* must run before i2s_init() -- see pcm-xtensa.c */
#endif
void i2s_set_frequency(unsigned long rate);
#ifdef HAVE_RECORDING
/* The receiver is a full ESP-IDF I2S channel created per recording session in
 * pcm-xtensa.c, so there is no rx init/start/stop to declare here. What the
 * capture path does need from this file is the rate the session must be created
 * at, and the shared clock pads. */
unsigned long i2s_rx_sample_rate(void);
/* The board shares one MCLK/BCLK/WS set between both codecs, so only one I2S port
 * may drive those pads at a time. Recording makes the receiver the master, so the
 * pads are handed over for the duration and given back afterwards. Sound only
 * because Rockbox makes play and record exclusive. */
void i2s_rx_pads_take(void);
void i2s_rx_pads_release(void);
#endif

#endif /* I2S_XTENSA_H */
