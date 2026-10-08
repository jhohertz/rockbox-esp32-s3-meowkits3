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

#define I2S1_BASE   0x6002D000
#define GDMA_BASE   0x6003F000
#define SYSTEM_BASE 0x600C0000

/* Peripheral clock gating. Both bits default to 0, so the bootloader does not
 * enable them: Rockbox has to. */
#define SYSTEM_PERIP_CLK_EN0 (SYSTEM_BASE + 0x18)
#define SYSTEM_PERIP_CLK_EN1 (SYSTEM_BASE + 0x1C)
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

#define CLKM_CONF_CLK_EN     (1u << 29)
#define CLKM_CONF_CLK_ACTIVE (1u << 26)
#define CLKM_CONF_CLK_SEL_XTAL 0   /* 0: XTAL, 1: APLL, 2: CLK160, 3: I2S_MCLK_in */

/* GDMA TX channel 0 */
#define GDMA_TX_CONF0   (GDMA_BASE + 0x60)
#define GDMA_TX_CONF1   (GDMA_BASE + 0x64)
#define GDMA_TX_INT_ST  (GDMA_BASE + 0x6C)
#define GDMA_TX_INT_ENA (GDMA_BASE + 0x70)
#define GDMA_TX_INT_CLR (GDMA_BASE + 0x74)

/* OUT_DONE (bit 0) fires when the last data pointed by one descriptor has been
 * transmitted to the peripheral, which is the point at which the buffer is safe to
 * refill. OUT_EOF (bit 1) fires earlier, when the data has only been read from
 * memory, so the buffer is still in the FIFO and must not be overwritten yet. */
#define GDMA_INT_DONE (1u << 0)
#define GDMA_INT_DSCR_ERR (1u << 2)
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

#define OWNER_CPU 0
#define OWNER_DMA 1

void i2s_init(void);
void i2s_set_frequency(unsigned long rate);

#endif /* I2S_XTENSA_H */
