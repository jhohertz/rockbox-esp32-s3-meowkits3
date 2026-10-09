/***************************************************************************
 * ESP32-S3 GPIO access.
 *
 * Register offsets verified against the ESP32-S3 SVD-derived header
 * (components/soc/esp32s3/register/soc/gpio_reg.h), not guessed.
 *
 * GPIO 32-53 are a second group with their own OUT/ENABLE/IN registers, so a
 * driver that only uses the low group silently ignores pins 40 and 41, which are
 * exactly the LCD MOSI and SCLK pins.
 ****************************************************************************/
#ifndef GPIO_XTENSA_H
#define GPIO_XTENSA_H

#include <stdint.h>

#define DR_REG_GPIO_BASE 0x60004000

#define GPIO_OUT          (DR_REG_GPIO_BASE + 0x04)
#define GPIO_OUT_W1TS     (DR_REG_GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC     (DR_REG_GPIO_BASE + 0x0C)
#define GPIO_OUT1         (DR_REG_GPIO_BASE + 0x10)
#define GPIO_OUT1_W1TS    (DR_REG_GPIO_BASE + 0x14)
#define GPIO_OUT1_W1TC    (DR_REG_GPIO_BASE + 0x18)

#define GPIO_ENABLE       (DR_REG_GPIO_BASE + 0x20)
#define GPIO_ENABLE_W1TS  (DR_REG_GPIO_BASE + 0x24)
#define GPIO_ENABLE_W1TC  (DR_REG_GPIO_BASE + 0x28)
#define GPIO_ENABLE1      (DR_REG_GPIO_BASE + 0x2C)
#define GPIO_ENABLE1_W1TS (DR_REG_GPIO_BASE + 0x30)
#define GPIO_ENABLE1_W1TC (DR_REG_GPIO_BASE + 0x34)

#define GPIO_IN           (DR_REG_GPIO_BASE + 0x3C)
#define GPIO_IN1          (DR_REG_GPIO_BASE + 0x40)

/* GPIO matrix: one 32-bit slot per pin.
 *   [8:0]   func_sel  - signal index, default 0x100 (SIG_GPIO_OUT_IDX = no signal)
 *   [9]     inv_sel
 *   [10]    oen_sel   - 1: output enable comes from the GPIO_ENABLE bit for this pin
 *   [11]    oen_inv_sel
 * func_sel is 9 bits, not 6: the GPSPI signals are 101-103, which a 6-bit mask
 * turns into 5-7. See components/soc/esp32s3/register/soc/gpio_struct.h.
 */
#define GPIO_FUNC_OUT_SEL(n) (DR_REG_GPIO_BASE + 0x554 + 4u * (n))
#define GPIO_OEN_FROM_ENABLE (1u << 10)

/* GPIO matrix input selection: one 32-bit slot per SIGNAL, not per pin.
 *   [5:0] in_sel - which GPIO feeds this signal
 *   [6]   inv_sel
 *   [7]   sel    - 1: take the input from the matrix rather than bypassing it
 * Offsets from ESP-IDF's gpio_reg.h (GPIO_FUNC0_IN_SEL_CFG_REG = base + 0x154).
 */
#define GPIO_FUNC_IN_SEL(n) (DR_REG_GPIO_BASE + 0x154 + 4u * (n))
#define GPIO_IN_FROM_MATRIX (1u << 7)

/* IO MUX: MCU_SEL is bits 14:12 and picks the pad function. GPIO1 and GPIO2
 * default to UART0 RX/TX, so a driver that wants them as plain GPIO has to write
 * 0 here. GPIO0 is at base + 0x04, one word per pin.
 */
#define DR_REG_IO_MUX_BASE 0x60009000
#define IO_MUX_PIN(n) (DR_REG_IO_MUX_BASE + 0x04 + 4u * (n))
#define IO_MUX_MCU_SEL (1u << 12)   /* MCU_SEL: 0 = GPIO function */
#define IO_MUX_FUN_IE  (1u << 9)    /* input enable of the pad */

/* Signal indices from soc/gpio_sig_map.h */
#define SIG_I2C0_SCL     89
#define SIG_I2C0_SDA     90

#define SIG_FSPICLK_OUT  101   /* GPSPI2 clock */
#define SIG_FSPIQ_OUT    102   /* GPSPI2 MOSI */
#define SIG_FSPID_OUT    103   /* GPSPI2 MISO */

/* I2S1 outputs */
#define SIG_I2S1_MCLK_OUT 21
#define SIG_I2S1O_BCK_OUT 28
#define SIG_I2S1O_WS_OUT  29
#define SIG_I2S1O_SD_OUT  30

static inline uint32_t gpio_read(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

static inline void gpio_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)reg = val;
}

static inline void gpio_set_direction(int pin, int output)
{
    if (pin < 32) {
        gpio_write(output ? GPIO_ENABLE_W1TS : GPIO_ENABLE_W1TC, 1u << pin);
    } else {
        gpio_write(output ? GPIO_ENABLE1_W1TS : GPIO_ENABLE1_W1TC, 1u << (pin - 32));
    }
}

static inline void gpio_write_pin(int pin, int value)
{
    if (pin < 32) {
        gpio_write(value ? GPIO_OUT_W1TS : GPIO_OUT_W1TC, 1u << pin);
    } else {
        gpio_write(value ? GPIO_OUT1_W1TS : GPIO_OUT1_W1TC, 1u << (pin - 32));
    }
}

static inline int gpio_read_pin(int pin)
{
    uint32_t in = (pin < 32) ? gpio_read(GPIO_IN) : gpio_read(GPIO_IN1);
    return (int)((in >> (pin < 32 ? pin : pin - 32)) & 1u);
}

/* Route a peripheral signal to a pin through the GPIO matrix. */
static inline void gpio_matrix_out(int pin, int signal)
{
    gpio_write(GPIO_FUNC_OUT_SEL(pin), ((uint32_t)signal & 0x1FF) | GPIO_OEN_FROM_ENABLE);
}

/* Route a pin's input to a peripheral signal. The slot is indexed by signal. */
static inline void gpio_matrix_in(int pin, int signal)
{
    gpio_write(GPIO_FUNC_IN_SEL(signal), ((uint32_t)pin & 0x3F) | GPIO_IN_FROM_MATRIX);
}

/* Pick the GPIO function for a pad, which is not the default for every pin. */
static inline void io_mux_select_gpio(int pin)
{
    uint32_t r = IO_MUX_PIN(pin);
    gpio_write(r, (gpio_read(r) & ~IO_MUX_MCU_SEL) | IO_MUX_FUN_IE);
}


#endif /* GPIO_XTENSA_H */
