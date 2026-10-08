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

/* GPIO matrix: one 32-bit slot per pin, OUT_SEL = signal index. */
#define GPIO_FUNC_OUT_SEL(n) (DR_REG_GPIO_BASE + 0x554 + 4u * (n))

/* GPSPI signal indices (soc/gpio_sig_map.h). SPI2 and SPI3 share them. */
#define SIG_SPI_CLK_OUT  66
#define SIG_SPI_Q_OUT    67   /* MOSI */

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
    gpio_write(GPIO_FUNC_OUT_SEL(pin), (uint32_t)signal & 0x3F);
}

#endif /* GPIO_XTENSA_H */
