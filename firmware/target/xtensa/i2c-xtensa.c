/***************************************************************************
 * I2C master peripheral for the ESP32-S3 (I2C0 at 0x60013000, SDA = GPIO1,
 * SCL = GPIO2).
 *
 * The reference firmware uses this peripheral rather than bit-banging, and the
 * schematic has 10K pull-ups on both lines (R30/R31 to EXP_3V3), so the peripheral
 * is the right choice. ESP32-S3 GPIO has no internal pull-up registers - they are
 * absent from the SVD, from ESP-IDF's gpio_reg.h and from soc_caps.h for this chip
 * - so those external pull-ups are load-bearing.
 *
 * The peripheral is a command-list machine. The CMD_Controller reads up to eight
 * command registers, executes them in order, and stops at STOP or END. Data to send
 * lives in TX RAM (32 x 8 bits) and received data lands in RX RAM. In master mode
 * the slave address and the R/W bit are part of TX RAM rather than a separate
 * register, so byte_num counts them.
 *
 * That shape is why the byte-at-a-time half of i2c.h is not implemented here. A
 * single command cannot be executed on its own, so i2c_inb() cannot return a byte
 * and i2c_getack() cannot report an ACK until the whole list has run. Everything
 * this target builds uses the transaction-level API, which maps cleanly: pca9557.c
 * and es8311.c call only i2c_init, i2c_readmem and i2c_writemem. If a future driver
 * needs the byte-level API it has to be written as whole transactions.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "i2c.h"
#include "gpio-xtensa.h"

#define DR_REG_I2C_BASE 0x60013000

#define I2C_SCL_LOW_PERIOD   (DR_REG_I2C_BASE + 0x00)
#define I2C_CTR              (DR_REG_I2C_BASE + 0x04)
#define I2C_SR               (DR_REG_I2C_BASE + 0x08)
#define I2C_TO               (DR_REG_I2C_BASE + 0x0C)
#define I2C_SLAVE_ADDR       (DR_REG_I2C_BASE + 0x10)
#define I2C_FIFO_ST          (DR_REG_I2C_BASE + 0x14)
#define I2C_FIFO_CONF        (DR_REG_I2C_BASE + 0x18)
#define I2C_INT_RAW          (DR_REG_I2C_BASE + 0x20)
#define I2C_INT_CLR          (DR_REG_I2C_BASE + 0x24)
#define I2C_SDA_HOLD         (DR_REG_I2C_BASE + 0x30)
#define I2C_SDA_SAMPLE       (DR_REG_I2C_BASE + 0x34)
#define I2C_SCL_HIGH_PERIOD  (DR_REG_I2C_BASE + 0x38)
#define I2C_SCL_START_HOLD   (DR_REG_I2C_BASE + 0x40)
#define I2C_SCL_RSTART_SETUP (DR_REG_I2C_BASE + 0x44)
#define I2C_SCL_STOP_HOLD    (DR_REG_I2C_BASE + 0x48)
#define I2C_SCL_STOP_SETUP   (DR_REG_I2C_BASE + 0x4C)
#define I2C_FILTER_CFG       (DR_REG_I2C_BASE + 0x50)
#define I2C_CLK_CONF         (DR_REG_I2C_BASE + 0x54)
#define I2C_SCL_SP_CONF      (DR_REG_I2C_BASE + 0x80)
#define I2C_COMD(n)          (DR_REG_I2C_BASE + 0x58 + 4u * (n))
#define I2C_TXFIFO(n)        (DR_REG_I2C_BASE + 0x100 + 4u * (n))
#define I2C_RXFIFO(n)        (DR_REG_I2C_BASE + 0x180 + 4u * (n))

/* CTR_REG */
#define CTR_SDA_FORCE_OUT (1u << 0)      /* 1: open drain output */
#define CTR_SCL_FORCE_OUT (1u << 1)
#define CTR_MS_MODE       (1u << 4)
#define CTR_TRANS_START   (1u << 5)
#define CTR_CONF_UPGATE   (1u << 11)     /* copy config into the I2C_SCLK domain */

/* INT_RAW_REG */
#define INT_TIME_OUT (1u << 8)
#define INT_NACK     (1u << 10)

/* op_code */
#define OP_WRITE  1
#define OP_STOP   2
#define OP_READ   3
#define OP_END    4
#define OP_RSTART 6

/* ack_value / ack_exp: 0 is ACK, 1 is NACK */
#define ACK 0
#define NACK 1

#define SYSTEM_BASE 0x600C0000
#define SYSTEM_PERIP_CLK_EN0 (SYSTEM_BASE + 0x18)
#define I2C_EXT0_CLK_EN (1u << 7)

#define I2C_SDA_PIN 1
#define I2C_SCL_PIN 2

/* The I2C module clock is XTAL_CLK (40 MHz) when SCLK_SEL is 0, not APB_CLK, so
 * the timing registers are in XTAL cycles and have nothing to do with
 * cpu_frequency. */
#define I2C_SRC_CLK  40000000u
#define I2C_BUS_FREQ 100000u

#define RAM_SIZE 32
#define CMD_SLOTS 8

static int ncmd;
static int ntx;

static void begin_list(void)
{
    ncmd = 0;
    ntx = 0;
}

/* Writing a command clears its DONE bit, which is what the controller looks for.
 * The list is always terminated by STOP, so slots after it are never read. */
static void add_cmd(unsigned opcode, unsigned nbytes,
                    unsigned ack_check, unsigned ack_expected, unsigned ack_value)
{
    if (ncmd >= CMD_SLOTS)
        return;

    gpio_write(I2C_COMD(ncmd), (nbytes & 0xFF) | (ack_check << 8) | (ack_expected << 9)
                               | (ack_value << 10) | (opcode << 11));
    ncmd++;
}

static void add_tx(unsigned char b)
{
    if (ntx >= RAM_SIZE)
        return;

    gpio_write(I2C_TXFIFO(ntx), b);
    ntx++;
}

static void add_write(unsigned char b)
{
    add_tx(b);
    add_cmd(OP_WRITE, 1, 1, ACK, 0);
}

/* Run the accumulated list. run() always terminates with STOP. */
static int run(void)
{
    add_cmd(OP_STOP, 0, 0, 0, 0);

    gpio_write(I2C_FIFO_CONF, gpio_read(I2C_FIFO_CONF) | (1u << 12) | (1u << 13));
    gpio_write(I2C_FIFO_CONF, gpio_read(I2C_FIFO_CONF) & ~((1u << 12) | (1u << 13)));

    gpio_write(I2C_INT_CLR, 0xFFFF);

    /* The registers live in the APB clock domain and the controller in the
     * asynchronous I2C_SCLK domain, so config has to be latched before triggering. */
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_CONF_UPGATE);
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_TRANS_START);

    for (int i = 0; i < 1000; i++) {
        unsigned raw = gpio_read(I2C_INT_RAW);

        if (raw & INT_NACK)
            return -1;
        if (raw & INT_TIME_OUT)
            return -1;
        if (raw & (1u << 7))          /* TRANS_COMPLETE */
            return 0;

        udelay(100);
    }

    return -1;
}

static void read_rx(unsigned char *buf, int count)
{
    for (int i = 0; i < count; i++)
        buf[i] = (unsigned char)gpio_read(I2C_RXFIFO(i));
}

/* A read is split so the master ACKs every byte but the last. */
static void add_read(int count)
{
    if (count > 1)
        add_cmd(OP_READ, (unsigned)(count - 1), 0, 0, ACK);
    add_cmd(OP_READ, 1, 0, 0, NACK);
}

static void set_timing(unsigned freq)
{
    unsigned clkm_div = I2C_SRC_CLK / (freq * 1024) + 1;
    unsigned sclk = I2C_SRC_CLK / clkm_div;
    unsigned half = sclk / freq / 2;
    /* Below 80 kHz a long wait-high makes the bus faster than requested; above it,
     * the shorter one is what holds the frequency. */
    unsigned wait_high = (freq >= 80000) ? (half / 2 - 2) : (half / 4);
    unsigned scl_high = half - wait_high;
    unsigned tout = 32u - (unsigned)__builtin_clz(5 * half) + 2;

    if (tout > 31)
        tout = 31;

    gpio_write(I2C_CLK_CONF, clkm_div - 1);                       /* SCLK_SEL = 0: XTAL */
    gpio_write(I2C_SCL_LOW_PERIOD, half - 1);
    gpio_write(I2C_SCL_HIGH_PERIOD, scl_high | (wait_high << 9));
    gpio_write(I2C_SDA_HOLD, half / 4 - 1);
    gpio_write(I2C_SDA_SAMPLE, half / 2 - 1);
    gpio_write(I2C_SCL_RSTART_SETUP, half - 1);
    gpio_write(I2C_SCL_STOP_SETUP, half - 1);
    gpio_write(I2C_SCL_START_HOLD, half - 1);
    gpio_write(I2C_SCL_STOP_HOLD, half - 1);
    gpio_write(I2C_TO, tout | (1u << 5));
}

/*
 * Bus recovery: nine SCL pulses so a slave that was interrupted mid-transaction can
 * finish, then a STOP. The peripheral generates the pulses itself when idle, which
 * is exactly the recovery the spec asks for, so there is no need to bit-bang them.
 */
static void i2c_bus_recovery(void)
{
    gpio_write(I2C_SCL_SP_CONF, (9u << 1) | 1u);   /* SCL_RST_SLV_NUM = 9, EN = 1 */

    for (int i = 0; i < 100; i++) {
        if (!(gpio_read(I2C_SCL_SP_CONF) & 1u))
            break;   /* hardware clears EN when the pulses are done */
        udelay(100);
    }

    if (gpio_read_pin(I2C_SDA_PIN) == 0)
        return;   /* still held low; a STOP cannot be generated while it is */

    begin_list();
    run();
}

void i2c_init(void)
{
    gpio_write(SYSTEM_PERIP_CLK_EN0, gpio_read(SYSTEM_PERIP_CLK_EN0) | I2C_EXT0_CLK_EN);

    /* GPIO1 and GPIO2 default to UART0 RX and TX, so the pad function has to be
     * changed before the matrix can use them as plain GPIO. */
    io_mux_select_gpio(I2C_SDA_PIN);
    io_mux_select_gpio(I2C_SCL_PIN);

    /* Output: the controller does not drive pads directly, signals have to be routed
     * through the matrix. oen_sel = 1 makes the GPIO_ENABLE bit the output enable. */
    gpio_matrix_out(I2C_SDA_PIN, SIG_I2C0_SDA);
    gpio_matrix_out(I2C_SCL_PIN, SIG_I2C0_SCL);
    /* Input: the controller also has to see the lines, and the matrix input slots are
     * indexed by signal rather than by pin. */
    gpio_matrix_in(I2C_SDA_PIN, SIG_I2C0_SDA);
    gpio_matrix_in(I2C_SCL_PIN, SIG_I2C0_SCL);

    gpio_set_direction(I2C_SDA_PIN, 1);
    gpio_set_direction(I2C_SCL_PIN, 1);

    set_timing(I2C_BUS_FREQ);

    /* Digital filter on both lines. The bus is loaded with about 100 pF per line and
     * 10K pull-ups, so a few module-clock cycles of threshold is cheap insurance
     * against ringing on the SCL edge. */
    gpio_write(I2C_FILTER_CFG, 4u | (4u << 4) | (1u << 8) | (1u << 9));

    /* Direct (non-FIFO) access to TX and RX RAM: each byte occupies a whole word at a
     * fixed address, which is simpler than the auto-advancing pointers and is what
     * double addressing requires anyway. */
    gpio_write(I2C_FIFO_CONF, 1u << 10);

    i2c_bus_recovery();
}

void i2c_begin(void)
{
    /* Single master, and every transaction here is self-contained. */
}

void i2c_end(void)
{
}

int i2c_write(int device, const unsigned char *buf, int count)
{
    if (count <= 0 || count + 1 > RAM_SIZE)
        return 0;

    begin_list();

    add_cmd(OP_RSTART, 0, 0, 0, 0);
    add_tx((unsigned char)((device << 1) & 0xFF));
    for (int i = 0; i < count; i++)
        add_tx(buf[i]);
    add_cmd(OP_WRITE, (unsigned)(count + 1), 1, ACK, 0);

    if (run() < 0)
        return -1;

    return count;
}

int i2c_read(int device, unsigned char *buf, int count)
{
    if (count <= 0 || count > RAM_SIZE)
        return 0;

    begin_list();

    add_cmd(OP_RSTART, 0, 0, 0, 0);
    add_write((unsigned char)(((device << 1) & 0xFF) | 1));   /* address with the R bit */
    add_read(count);

    if (run() < 0)
        return -1;

    read_rx(buf, count);

    return count;
}

int i2c_readmem(int device, int address, unsigned char *buf, int count)
{
    if (count <= 0 || count > RAM_SIZE)
        return 0;

    begin_list();

    /* Seven commands: RSTART, WRITE(address+register), RSTART, WRITE(address+R),
     * READ(count-1), READ(1), STOP. */
    add_cmd(OP_RSTART, 0, 0, 0, 0);
    add_tx((unsigned char)((device << 1) & 0xFF));
    add_tx((unsigned char)address);
    add_cmd(OP_WRITE, 2, 1, ACK, 0);

    add_cmd(OP_RSTART, 0, 0, 0, 0);
    add_write((unsigned char)(((device << 1) & 0xFF) | 1));
    add_read(count);

    if (run() < 0)
        return -1;

    read_rx(buf, count);

    return count;
}

int i2c_writemem(int device, int address, const unsigned char *buf, int count)
{
    if (count <= 0 || count + 2 > RAM_SIZE)
        return 0;

    begin_list();

    add_cmd(OP_RSTART, 0, 0, 0, 0);
    add_tx((unsigned char)((device << 1) & 0xFF));
    add_tx((unsigned char)address);
    for (int i = 0; i < count; i++)
        add_tx(buf[i]);
    add_cmd(OP_WRITE, (unsigned)(count + 2), 1, ACK, 0);

    if (run() < 0)
        return -1;

    return count;
}
