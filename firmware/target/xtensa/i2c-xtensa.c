/***************************************************************************
 * Bit-banged I2C for the ESP32-S3 (SDA = GPIO1, SCL = GPIO2).
 *
 * Chosen over the I2C master peripheral because that peripheral is a command
 * FSM (you program a transaction descriptor and trigger it), which is a poor fit
 * for Rockbox's byte-at-a-time i2c.h API. Bit-banging needs only the GPIO
 * registers, which are verified.
 *
 * Both lines are open-drain: "write 1" means release the line (switch to input),
 * "write 0" means drive low.
 *
 * NOTE ON PULL-UPS: the ESP32-S3 GPIO group has no internal pull-up registers —
 * they are absent from the SVD-derived header and from ESP-IDF's gpio_reg.h, and
 * soc_caps.h has no SOC_GPIO_SUPPORT_PULL_UP for this chip. The reference
 * firmware calls GPIO_PULLUP_ENABLE, but that cannot be relied on here. External
 * pull-ups on SDA/SCL are required for this driver to work.
 ****************************************************************************/
#include "config.h"
#include "i2c.h"
#include "gpio-xtensa.h"

#define I2C_SDA 1
#define I2C_SCL 2

/* ~100 kHz: half-period 5 us. At 240 MHz a loop iteration is roughly 4 cycles,
 * so ~300 iterations. I2C tolerates a slow clock, so this is deliberately
 * conservative rather than cycle-exact. */
#define I2C_DELAY_ITERS 300

static void i2c_delay(void)
{
    for (int i = 0; i < I2C_DELAY_ITERS; i++) {
        asm volatile ("" ::: "memory");
    }
}

static void sda_out(int value)
{
    gpio_set_direction(I2C_SDA, 1);
    gpio_write_pin(I2C_SDA, value);
}

static void sda_in(void)
{
    gpio_set_direction(I2C_SDA, 0);
}

static int sda_read(void)
{
    return gpio_read_pin(I2C_SDA);
}

static void scl_out(int value)
{
    gpio_set_direction(I2C_SCL, 1);
    gpio_write_pin(I2C_SCL, value);
}

void i2c_init(void)
{
    /* Idle bus: lines released. */
    sda_in();
    scl_out(1);
}

void i2c_begin(void)
{
    /* Single master, no bus lock needed. */
}

void i2c_end(void)
{
}

void i2c_start(void)
{
    sda_in();
    scl_out(1);
    i2c_delay();

    if (sda_read() == 0) {
        /* SDA held low while SCL is high: a slave is driving the bus. */
        return;
    }

    sda_out(0);   /* START: SDA falls while SCL is high */
    i2c_delay();
    scl_out(0);
    i2c_delay();
}

void i2c_stop(void)
{
    scl_out(0);
    sda_out(0);
    i2c_delay();
    scl_out(1);   /* STOP: SDA rises while SCL is high */
    i2c_delay();
    sda_in();
}

void i2c_ack(int bit)
{
    scl_out(0);
    sda_out(bit);        /* master drives the ACK bit */
    i2c_delay();
    scl_out(1);
    i2c_delay();
    scl_out(0);
    sda_in();
    i2c_delay();
}

int i2c_getack(void)
{
    int ack;

    sda_in();
    scl_out(0);
    i2c_delay();
    scl_out(1);
    i2c_delay();

    ack = (sda_read() == 0) ? 1 : 0;

    scl_out(0);
    i2c_delay();

    return ack;
}

/* i2c.h declares this as void: the caller checks i2c_getack() separately. Some
 * target variants return int, which conflicts with the header. */
void i2c_outb(unsigned char byte)
{
    for (int i = 7; i >= 0; i--) {
        sda_out((byte >> i) & 1);
        i2c_delay();
        scl_out(1);
        i2c_delay();
        scl_out(0);
        i2c_delay();
    }
}

static int outb_ack(unsigned char byte)
{
    i2c_outb(byte);
    return i2c_getack() ? 1 : -1;
}

unsigned char i2c_inb(int ack)
{
    int i;
    unsigned char byte = 0;

    sda_in();

    for (i = 7; i >= 0; i--) {
        scl_out(0);
        i2c_delay();
        scl_out(1);
        i2c_delay();
        byte = (unsigned char)((byte << 1) | sda_read());
    }

    i2c_ack(ack);

    return byte;
}

int i2c_write(int device, const unsigned char *buf, int count)
{
    int i;

    if (count <= 0)
        return 0;

    i2c_start();

    if (outb_ack((device << 1) | 0) < 0) {
        i2c_stop();
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (outb_ack(buf[i]) < 0) {
            i2c_stop();
            return -1;
        }
    }

    i2c_stop();

    return count;
}

int i2c_read(int device, unsigned char *buf, int count)
{
    int i;

    if (count <= 0)
        return 0;

    i2c_start();

    if (outb_ack((device << 1) | 1) < 0) {
        i2c_stop();
        return -1;
    }

    for (i = 0; i < count; i++) {
        buf[i] = i2c_inb(i < count - 1);   /* NACK on the last byte */
    }

    i2c_stop();

    return count;
}

int i2c_readmem(int device, int address, unsigned char *buf, int count)
{
    unsigned char addr = (unsigned char)address;

    i2c_start();

    if (outb_ack((device << 1) | 0) < 0) {
        i2c_stop();
        return -1;
    }

    if (outb_ack(addr) < 0) {
        i2c_stop();
        return -1;
    }

    i2c_start();

    if (outb_ack((device << 1) | 1) < 0) {
        i2c_stop();
        return -1;
    }

    for (int i = 0; i < count; i++) {
        buf[i] = i2c_inb(i < count - 1);
    }

    i2c_stop();

    return count;
}

int i2c_writemem(int device, int address, const unsigned char *buf, int count)
{
    unsigned char addr = (unsigned char)address;

    i2c_start();

    if (outb_ack((device << 1) | 0) < 0) {
        i2c_stop();
        return -1;
    }

    if (outb_ack(addr) < 0) {
        i2c_stop();
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (outb_ack(buf[i]) < 0) {
            i2c_stop();
            return -1;
        }
    }

    i2c_stop();

    return count;
}
