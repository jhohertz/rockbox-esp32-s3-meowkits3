/***************************************************************************
 * I2C master peripheral for the ESP32-S3 (I2C0 at 0x60013000, SDA = GPIO1,
 * SCL = GPIO2).
 *
 * The reference firmware uses this peripheral rather than bit-banging, and the
 * schematic has 10K pull-ups on both lines (R30/R31 to EXP_3V3), so the
 * peripheral is the right choice. ESP32-S3 does have internal pull-ups, but they
 * live in the IO MUX (FUN_PU, bit 8 of the per-pad register), not in the GPIO
 * block - searching gpio_reg.h or gpio_struct.h finds nothing, which is how it
 * is easy to conclude the chip has none.
 *
 * The peripheral is a command-list machine. The CMD_Controller reads up to eight
 * command registers, executes them in order, and stops at STOP or END. Data to
 * send lives in TX RAM (32 x 8 bits) and received data lands in RX RAM. In
 * master mode the slave address and the R/W bit are part of TX RAM rather than a
 * separate register, so byte_num counts them. Each WRITE command consumes
 * byte_num bytes from the current TX RAM pointer, so a transaction that writes a
 * register address and then restarts to read lays out the address bytes
 * consecutively and lets the pointer advance.
 *
 * That shape is why the byte-at-a-time half of i2c.h is not implemented here. A
 * single command cannot be executed on its own, so i2c_inb() cannot return a
 * byte and i2c_getack() cannot report an ACK until the whole list has run.
 *
 * Everything this target needs is transaction-level, and the asynchronous core
 * describes transactions directly: an i2c_descriptor is one START ... STOP
 * sequence. So the synchronous entry points are built on i2c-async rather than
 * implemented separately. There is then exactly one path to the peripheral and
 * one in-flight transaction, which is what makes it safe for the touchscreen to
 * poll the bus while the codec or the PMU is doing something else.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "i2c.h"
#include "i2c-async.h"
#include "gpio-xtensa.h"
#include "xtensa.h"

#define DR_REG_I2C_BASE 0x60013000

#define I2C_SCL_LOW_PERIOD   (DR_REG_I2C_BASE + 0x00)
#define I2C_CTR              (DR_REG_I2C_BASE + 0x04)
#define I2C_SR               (DR_REG_I2C_BASE + 0x08)
#define I2C_TO               (DR_REG_I2C_BASE + 0x0C)
#define I2C_SLAVE_ADDR       (DR_REG_I2C_BASE + 0x10)
#define I2C_FIFO_ST          (DR_REG_I2C_BASE + 0x14)
#define I2C_FIFO_CONF        (DR_REG_I2C_BASE + 0x18)
#define I2C_DATA             (DR_REG_I2C_BASE + 0x1C)
#define I2C_INT_RAW          (DR_REG_I2C_BASE + 0x20)
#define I2C_INT_CLR          (DR_REG_I2C_BASE + 0x24)
#define I2C_INT_ENA          (DR_REG_I2C_BASE + 0x28)
#define I2C_INT_ST           (DR_REG_I2C_BASE + 0x2C)
#define I2C_SDA_HOLD         (DR_REG_I2C_BASE + 0x30)
#define I2C_SDA_SAMPLE       (DR_REG_I2C_BASE + 0x34)
#define I2C_SCL_HIGH_PERIOD  (DR_REG_I2C_BASE + 0x38)
#define I2C_SCL_START_HOLD   (DR_REG_I2C_BASE + 0x40)
#define I2C_SCL_RSTART_SETUP (DR_REG_I2C_BASE + 0x44)
#define I2C_SCL_STOP_HOLD    (DR_REG_I2C_BASE + 0x48)
#define I2C_SCL_STOP_SETUP   (DR_REG_I2C_BASE + 0x4C)
#define I2C_FILTER_CFG       (DR_REG_I2C_BASE + 0x50)
#define I2C_CLK_CONF         (DR_REG_I2C_BASE + 0x54)
#define I2C_COMD(n)          (DR_REG_I2C_BASE + 0x58 + 4u * (n))
#define I2C_SCL_SP_CONF      (DR_REG_I2C_BASE + 0x80)
#define I2C_TXFIFO(n)        (DR_REG_I2C_BASE + 0x100 + 4u * (n))
#define I2C_RXFIFO(n)        (DR_REG_I2C_BASE + 0x180 + 4u * (n))

/* CTR_REG */
#define CTR_SDA_FORCE_OUT (1u << 0)      /* 1: open drain output */
#define CTR_SCL_FORCE_OUT (1u << 1)
#define CTR_MS_MODE       (1u << 4)
#define CTR_TRANS_START   (1u << 5)
#define CTR_CONF_UPGATE   (1u << 11)     /* copy config into the I2C_SCLK domain */

/* INT_RAW_REG */
#define INT_RXFIFO_WM  (1u << 0)
#define INT_TXFIFO_WM  (1u << 1)
#define INT_END_DET    (1u << 3)
#define INT_ARB_LOST   (1u << 5)
#define INT_TRANS_DONE (1u << 7)
#define INT_TIME_OUT   (1u << 8)
#define INT_NACK       (1u << 10)

/* Interrupts that mean something to a master transaction. */
#define INT_MASTER_MASK (INT_TRANS_DONE | INT_TIME_OUT | INT_NACK | INT_ARB_LOST)

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

#define RAM_SIZE  32
#define CMD_SLOTS 8

static int ncmd;
static int ntx;

/* The descriptor the hardware is currently working on, and where the received
 * bytes have to be copied once the transaction completes. */
static unsigned char *rx_buf;
static int rx_count;

/* Set when a descriptor could not be programmed at all, so the ISR reports the
 * failure instead of the nominal success of the dummy list. */
static int forced_status;

static void begin_list(void)
{
    ncmd = 0;
    ntx = 0;
}

/* Writing a command clears its DONE bit, which is what the controller looks for. */
static bool add_cmd(unsigned opcode, unsigned nbytes,
                    unsigned ack_check, unsigned ack_expected, unsigned ack_value)
{
    if (ncmd >= CMD_SLOTS)
        return false;

    gpio_write(I2C_COMD(ncmd), (nbytes & 0xFF) | (ack_check << 8) | (ack_expected << 9)
                               | (ack_value << 10) | (opcode << 11));
    ncmd++;
    return true;
}

static bool add_tx(unsigned char b)
{
    if (ntx >= RAM_SIZE)
        return false;

    gpio_write(I2C_TXFIFO(ntx), b);
    ntx++;
    return true;
}

static bool build_write(int device, const unsigned char *a, int na,
                        const unsigned char *b, int nb)
{
    begin_list();

    add_cmd(OP_RSTART, 0, 0, 0, 0);
    if (!add_tx((unsigned char)((device << 1) & 0xFF)))
        return false;

    for (int i = 0; i < na; i++)
        if (!add_tx(a[i]))
            return false;

    for (int i = 0; i < nb; i++)
        if (!add_tx(b[i]))
            return false;

    return add_cmd(OP_WRITE, (unsigned)ntx, 1, ACK, 0);
}

/* A read is split so the master ACKs every byte but the last. */
static bool add_read(int count)
{
    if (count > 1)
        if (!add_cmd(OP_READ, (unsigned)(count - 1), 0, 0, ACK))
            return false;

    return add_cmd(OP_READ, 1, 0, 0, NACK);
}

static bool build_read(int device, const unsigned char *pre, int npre, int count)
{
    begin_list();

    add_cmd(OP_RSTART, 0, 0, 0, 0);
    if (!add_tx((unsigned char)((device << 1) & 0xFF)))
        return false;

    for (int i = 0; i < npre; i++)
        if (!add_tx(pre[i]))
            return false;

    if (!add_cmd(OP_WRITE, (unsigned)ntx, 1, ACK, 0))
        return false;

    /* Restart, then send the address again with the R bit set. The pointer is
     * already past the bytes the first WRITE consumed, so this byte is the next
     * one in RAM. */
    add_cmd(OP_RSTART, 0, 0, 0, 0);
    if (!add_tx((unsigned char)(((device << 1) & 0xFF) | 1)))
        return false;
    if (!add_cmd(OP_WRITE, 1, 1, ACK, 0))
        return false;

    return add_read(count);
}

/* Latch the programmed list and trigger the controller. The registers live in the
 * APB clock domain and the controller in the asynchronous I2C_SCLK domain, so the
 * config has to be copied across before the transaction is started. */
static void start_list(bool stop)
{
    if (!add_cmd(stop ? OP_STOP : OP_END, 0, 0, 0, 0))
        return;

    /* Reset both FIFOs so the TX pointer starts at zero. */
    gpio_write(I2C_FIFO_CONF, gpio_read(I2C_FIFO_CONF) | (1u << 12) | (1u << 13));
    gpio_write(I2C_FIFO_CONF, gpio_read(I2C_FIFO_CONF) & ~((1u << 12) | (1u << 13)));

    gpio_write(I2C_INT_CLR, 0xFFFF);

    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_CONF_UPGATE);
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_TRANS_START);
}

/* Program the descriptor and hand it to the controller. Completion is reported by
 * i2c_irq(), not by polling. */
void __i2c_async_submit(int bus, i2c_descriptor *desc)
{
    (void)bus;

    bool ok;

    if (desc->tran_mode == I2C_WRITE)
        ok = build_write(desc->slave_addr, desc->buffer[0], desc->count[0],
                         desc->buffer[1], desc->count[1]);
    else
        ok = build_read(desc->slave_addr, desc->count[0] ? desc->buffer[0] : NULL,
                        desc->count[0], desc->count[1]);

    rx_buf = (desc->tran_mode == I2C_READ) ? (unsigned char *)desc->buffer[1] : NULL;
    rx_count = (desc->tran_mode == I2C_READ) ? desc->count[1] : 0;

    if (!ok) {
        /* Over budget for the 32-byte RAM or the eight command slots. Run a
         * minimal list so the queue still advances, and report the failure. */
        forced_status = I2C_STATUS_ERROR;
        rx_buf = NULL;
        rx_count = 0;
        begin_list();
        start_list(true);
        return;
    }

    forced_status = I2C_STATUS_OK;
    start_list((desc->bus_cond & I2C_STOP) != 0);
}

void i2c_irq(void)
{
    unsigned raw = gpio_read(I2C_INT_RAW);
    int status;

    if (raw & (INT_NACK | INT_TIME_OUT | INT_ARB_LOST))
        status = I2C_STATUS_ERROR;
    else if (raw & INT_TRANS_DONE)
        status = forced_status;
    else
        return;   /* FIFO watermark bits; nothing to report */

    gpio_write(I2C_INT_CLR, raw);

    if (status == I2C_STATUS_OK && rx_count > 0) {
        for (int i = 0; i < rx_count; i++)
            rx_buf[i] = (unsigned char)gpio_read(I2C_RXFIFO(i));
    }

    rx_count = 0;
    rx_buf = NULL;

    /* The async core keeps queue state that must not be touched by another
     * transaction starting in the meantime. */
    int irq = disable_irq_save();
    __i2c_async_complete_callback(0, status);
    restore_irq(irq);
}

/* Polled completion, used only by bus recovery, which runs before any
 * asynchronous transaction can be in flight. */
static int run_sync(void)
{
    gpio_write(I2C_INT_ENA, 0);
    start_list(true);

    for (int i = 0; i < 1000; i++) {
        unsigned raw = gpio_read(I2C_INT_RAW);

        if (raw & (INT_NACK | INT_TIME_OUT | INT_ARB_LOST))
            return -1;
        if (raw & INT_TRANS_DONE)
            return 0;

        udelay(100);
    }

    return -1;
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
 * Bus recovery: nine SCL pulses so a slave that was interrupted mid-transaction
 * can finish, then a STOP. The peripheral generates the pulses itself when idle,
 * which is exactly the recovery the spec asks for, so there is no need to
 * bit-bang them.
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
    run_sync();
}

void i2c_init(void)
{
    static bool async_ready;

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

    if (!async_ready) {
        __i2c_async_init();
        /* Route the controller's interrupt to a free level-1 slot. Slots 1, 2, 3 and
         * 5 are taken by the tick, the audio GDMA channel, SDHOST and the recording
         * GDMA channel; slot 4 is the WiFi baseband and 6/7 are the core timer and
         * the software interrupt. */
        gpio_write(XT_INTERRUPT_CORE0_BASE + 4 * XT_SRC_I2C0, XT_SLOT_I2C0);
        set_intenable(get_intenable() | (1u << XT_SLOT_I2C0));
        async_ready = true;
    }

    /* Recovery polls rather than interrupts, and a stale TRANS_COMPLETE bit would
     * otherwise complete a transaction that never started. The enable is restored
     * afterwards, on every call, because pca9557_init() re-enters i2c_init() to
     * recover a wedged bus. */
    i2c_bus_recovery();
    gpio_write(I2C_INT_ENA, INT_MASTER_MASK);
}

/* Blocking wrapper: queue one descriptor and wait for the interrupt to report it.
 * This is the only way the synchronous API reaches the hardware, so the async core
 * is what serialises every transaction on the bus. */
static void sync_callback(int status, i2c_descriptor *d)
{
    semaphore_release((struct semaphore *)d->arg);
    d->arg = status;
}

static int sync_run(i2c_descriptor *desc)
{
    struct semaphore sem;

    semaphore_init(&sem, 1, 0);
    desc->callback = sync_callback;
    desc->arg = (intptr_t)&sem;
    desc->next = NULL;

    if (i2c_async_queue(0, TIMEOUT_BLOCK, I2C_Q_ADD, 0, desc) != I2C_RC_OK)
        return I2C_STATUS_ERROR;

    semaphore_wait(&sem, TIMEOUT_BLOCK);
    return (int)desc->arg;
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
    i2c_descriptor desc;

    if (count <= 0)
        return 0;

    desc.slave_addr = device;
    desc.bus_cond = I2C_START | I2C_STOP;
    desc.tran_mode = I2C_WRITE;
    desc.buffer[0] = (void *)buf;
    desc.count[0] = count;
    desc.buffer[1] = NULL;
    desc.count[1] = 0;

    return sync_run(&desc) == I2C_STATUS_OK ? count : -1;
}

int i2c_read(int device, unsigned char *buf, int count)
{
    i2c_descriptor desc;

    if (count <= 0)
        return 0;

    desc.slave_addr = device;
    desc.bus_cond = I2C_START | I2C_STOP;
    desc.tran_mode = I2C_READ;
    desc.buffer[0] = NULL;
    desc.count[0] = 0;
    desc.buffer[1] = buf;
    desc.count[1] = count;

    return sync_run(&desc) == I2C_STATUS_OK ? count : -1;
}

int i2c_readmem(int device, int address, unsigned char *buf, int count)
{
    i2c_descriptor desc;
    unsigned char reg = (unsigned char)address;

    if (count <= 0)
        return 0;

    desc.slave_addr = device;
    desc.bus_cond = I2C_START | I2C_STOP;
    desc.tran_mode = I2C_READ;
    desc.buffer[0] = &reg;
    desc.count[0] = 1;
    desc.buffer[1] = buf;
    desc.count[1] = count;

    return sync_run(&desc) == I2C_STATUS_OK ? count : -1;
}

int i2c_writemem(int device, int address, const unsigned char *buf, int count)
{
    i2c_descriptor desc;
    unsigned char reg = (unsigned char)address;

    if (count <= 0)
        return 0;

    desc.slave_addr = device;
    desc.bus_cond = I2C_START | I2C_STOP;
    desc.tran_mode = I2C_WRITE;
    desc.buffer[0] = &reg;
    desc.count[0] = 1;
    desc.buffer[1] = (void *)buf;
    desc.count[1] = count;

    return sync_run(&desc) == I2C_STATUS_OK ? count : -1;
}
