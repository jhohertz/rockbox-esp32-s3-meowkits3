#include "config.h"
#include "system.h"   /* mdelay */
#include "i2c.h"
#include "pca9557.h"
#include "bootlog.h"

/* The reference firmware reads the output register before every write. That is two
 * I2C transactions per pin change, which is fine for control signals but wasteful.
 * We keep a local copy instead — safe as long as nothing else writes the register,
 * which is true here since Rockbox owns the expander. */
static unsigned char out_reg;
static unsigned char config_reg;

static unsigned i2c_rd(unsigned reg) { return *(volatile unsigned *)reg; }

static int read_reg(unsigned char reg, unsigned char *value)
{
    return i2c_readmem(PCA9557_ADDR, reg, value, 1) == 1;
}

static int write_reg(unsigned char reg, unsigned char value)
{
    int r = i2c_writemem(PCA9557_ADDR, reg, &value, 1);

    return r == 1;
}

int pca9557_status(void)
{
    unsigned char v;

    if (!read_reg(PCA9557_REG_INPUT, &v))
        return -1;

    return v;
}

int pca9557_write(int pin, int value)
{
    out_reg = (unsigned char)((out_reg & ~(1 << pin)) | (value ? (1 << pin) : 0));
    return write_reg(PCA9557_REG_OUTPUT, out_reg);
}

int pca9557_read(int pin)
{
    unsigned char v;

    if (!read_reg(PCA9557_REG_INPUT, &v))
        return -1;

    return (v >> pin) & 1;
}

/* The reference firmware verifies the expander actually answers before relying on it,
 * and retries with a bus recovery if not. i2c_init() performs the recovery, so the
 * retry uses only the shared I2C API rather than a target-local symbol. */
void pca9557_init(void)
{
    unsigned char v;
    boot_trace('B');
    int ok = 0;

    for (int attempt = 0; attempt < 5; attempt++) {
        if (read_reg(PCA9557_REG_CONFIG, &v)) {
            ok = 1;
            break;
        }

        i2c_init();   /* bus recovery */
        mdelay(100);
    }

    if (!ok)
        return;

    config_reg = v;

    /* The four control lines are outputs. */
    config_reg &= ~((1 << IOEXP_LCD_RST) | (1 << IOEXP_LCD_CS) |
                    (1 << IOEXP_FLASH_CS) | (1 << IOEXP_PA_EN) |
                    (1 << IOEXP_TOUCH_RST));

    write_reg(PCA9557_REG_CONFIG, config_reg);

    /* Polarity inversion off, as the reference does. */
    write_reg(PCA9557_REG_INVERSION, 0x00);

    if (!read_reg(PCA9557_REG_OUTPUT, &v))
        v = 0;

    out_reg = v;

    /* Confirm the config write took effect: the pins we just made outputs must
     * read back as outputs (bit clear in the config register).
     */
    if (read_reg(PCA9557_REG_CONFIG, &v)) {
        boot_trace('k');
        boot_trace_hex(v);
    }
    boot_trace('K');
}
