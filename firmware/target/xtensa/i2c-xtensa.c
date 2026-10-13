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
 * Everything this target needs is transaction-level, and a transaction is atomic:
 * the controller either runs the whole list or it does not. The synchronous entry
 * points therefore program the list, start it and poll for TRANS_COMPLETE. There
 * is one path to the peripheral and one in-flight transaction, which is what makes
 * it safe for the touchscreen to poll the bus while the codec or the PMU is doing
 * something else.
 *
 * The one bit that decides whether a transaction ACKs at all is the GPIO pad
 * driver. TRM 27.4.6 requires an open-drain master to have both I2C_SDA_FORCE_OUT /
 * I2C_SCL_FORCE_OUT set *and* the pad configured for open drain; with a push-pull
 * pad the master holds SDA high through the acknowledge slot and every device
 * reports NACK.
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "i2c.h"
#include "i2c-async.h"   /* i2c_descriptor definition */
#include "gpio-xtensa.h"
#include "xtensa.h"
#include "bootlog.h"

#define DR_REG_I2C_BASE 0x60013000
#define SYSTEM_PERIP_RST_EN0 0x600C0020
#define I2C_EXT0_RST (1u << 7)

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
#define CTR_ARB_EN        (1u << 9)     /* default 1; ESP-IDF clears it for master */
#define CTR_FSM_RST       (1u << 10)     /* reset the command FSM */
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

/* TX RAM is written through I2C_DATA in FIFO mode; hardware advances the write
 * pointer. The address is the same register used for reading RX RAM.
 */
static bool add_tx(unsigned char b)
{
    if (ntx >= RAM_SIZE)
        return false;

    gpio_write(I2C_DATA, b);
    ntx++;
    return true;
}

static bool build_write(int device, const unsigned char *a, int na,
                        const unsigned char *b, int nb)
{
    begin_list();

    if (!add_cmd(OP_RSTART, 0, 0, 0, 0))
        return false;
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
/* RX RAM is 32 words, one received byte per word, and byte_num is eight bits. A
 * longer read would wrap the RAM and report success on garbage. */
static bool add_read(int count)
{
    if (count > RAM_SIZE)
        return false;

    if (count > 1)
        if (!add_cmd(OP_READ, (unsigned)(count - 1), 0, 0, ACK))
            return false;

    return add_cmd(OP_READ, 1, 0, 0, NACK);
}

static bool build_read(int device, const unsigned char *pre, int npre, int count)
{
    begin_list();

    if (!add_cmd(OP_RSTART, 0, 0, 0, 0))
        return false;
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
    if (!add_cmd(OP_RSTART, 0, 0, 0, 0))
        return false;
    if (!add_tx((unsigned char)(((device << 1) & 0xFF) | 1)))
        return false;
    if (!add_cmd(OP_WRITE, 1, 1, ACK, 0))
        return false;

    return add_read(count);
}

/* Latch the programmed list and trigger the controller. The registers live in the
 * APB clock domain and the controller in the asynchronous I2C_SCLK domain, so the
 * config has to be copied across before the transaction is started. The caller is
 * responsible for the terminating STOP or END command. */
static void start_list(void)
{
    /* Reset both FIFOs so the TX pointer starts at zero. */
    gpio_write(I2C_FIFO_CONF, gpio_read(I2C_FIFO_CONF) | (1u << 12) | (1u << 13));
    gpio_write(I2C_FIFO_CONF, gpio_read(I2C_FIFO_CONF) & ~((1u << 12) | (1u << 13)));

    gpio_write(I2C_INT_CLR, 0xFFFF);

    /* FSM_RST is a write trigger like TRANS_START: pulse it so the command pointer
     * starts at slot 0 rather than wherever a previous list left it.
     */
    /* ESP-IDF's master init clears arbitration_en (default 1). Left set, the
     * controller compares the driven SDA against the sampled line and reports
     * ARB_LOST on any stretch, which aborts the list. */
    /* CTR is written whole, so the second write clears arbitration_en (default 1)
     * as well as fsm_rst. ESP-IDF's master init does the same via
     * i2c_ll_enable_arbitration(hw, false): with arbitration on, the controller
     * compares the driven SDA against the sampled line and aborts the list on any
     * slave stretch. */
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_FSM_RST);
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE);

    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_CONF_UPGATE);
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE | CTR_CONF_UPGATE | CTR_TRANS_START);
}

/* The CMD_Controller is a one-shot machine: it reads the eight command registers
 * from slot 0, executes them in order and stops at STOP or END. A transaction is
 * therefore atomic, and the only way to drive it is to program the whole list,
 * latch it and wait for TRANS_COMPLETE.
 */
static int forced_status;
static volatile int cb_done;
static volatile int cb_status;


/*
 * The transaction is polled to completion in the calling thread.
 *
 * The esp32s3 CMD_Controller is a one-shot machine: it executes the eight command
 * slots in order and stops at STOP or END. A transaction is short - a 32-byte read
 * at 100 kHz is under 4 ms - so polling costs nothing that an interrupt would save,
 * and it keeps the whole I2C path out of the level-1 handler, which already carries
 * the tick and the audio DMA. The controller interrupt is therefore left masked in
 * i2c_init().
 */
/* The poll has to fit inside the tick: a 32-byte transaction at 100 kHz needs
 * under 4 ms, so 5 ms is generous. A 60 ms bound turns every failed read into a
 * starvation of the main thread, because the tick fires every 10 ms.
 */
static int wait_complete(void)
{
    for (int i = 0; i < 1000; i++) {
        unsigned raw = gpio_read(I2C_INT_RAW);

        if (raw & INT_TRANS_DONE) {
            gpio_write(I2C_INT_CLR, raw);
            return (raw & (INT_NACK | INT_ARB_LOST)) ? I2C_STATUS_ERROR : forced_status;
        }
        udelay(10);
    }
    gpio_write(I2C_INT_CLR, 0xFFFF);
    return I2C_STATUS_TIMEOUT;
}

void __i2c_async_submit(int bus, i2c_descriptor *desc)
{
    bool ok;
    int status;
    gpio_write(I2C_INT_CLR, 0xFFFF);

    if (desc->tran_mode == I2C_WRITE)
        ok = build_write(desc->slave_addr, desc->buffer[0], desc->count[0],
                         desc->buffer[1], desc->count[1]);
    else
        ok = build_read(desc->slave_addr, desc->count[0] ? desc->buffer[0] : NULL,
                        desc->count[0], desc->count[1]);

    if (!ok) {
        forced_status = I2C_STATUS_ERROR;
        begin_list();
        add_cmd(OP_STOP, 0, 0, 0, 0);
        start_list();
        status = I2C_STATUS_ERROR;
    } else {
        if (!add_cmd((desc->bus_cond & I2C_STOP) ? OP_STOP : OP_END, 0, 0, 0, 0)) {
            forced_status = I2C_STATUS_ERROR;
            status = I2C_STATUS_ERROR;
        } else {
            forced_status = I2C_STATUS_OK;
            start_list();
            status = wait_complete();

            if (status == I2C_STATUS_OK && desc->tran_mode == I2C_READ &&
                desc->count[1] > 0) {
                /* RX RAM is read through I2C_DATA in FIFO mode; the read pointer
                 * auto-advances, so each read returns the next received byte.
                 */
                unsigned char *buf = (unsigned char *)desc->buffer[1];

                for (int i = 0; i < desc->count[1]; i++)
                    buf[i] = (unsigned char)(gpio_read(I2C_DATA) & 0xffu);
            }
        }
    }

    /* Completion is inline: the queue is drained before this returns, so a caller
     * waiting on the semaphore is released by the same call that queued the work.
     */
    __i2c_async_complete_callback(bus, status);
}

/* Kept for the dispatch: the controller interrupt is masked, so this should never
 * run. If it ever does, clearing the pending bits is the safe response.
 */
void ICODE_ATTR i2c_irq(void)
{
    unsigned raw = gpio_read(I2C_INT_RAW);

    if (raw)
        gpio_write(I2C_INT_CLR, raw);
}

/*
 * Bus recovery: nine SCL pulses so a slave that was interrupted mid-transaction
 * can finish. The peripheral generates them itself when idle, which is exactly
 * the recovery the spec asks for, so there is no need to bit-bang them. The
 * enable bit is self-clearing once the pulses have been emitted.
 */
static void i2c_bus_recovery(void)
{
    gpio_write(I2C_INT_CLR, 0xFFFF);
    gpio_write(I2C_SCL_SP_CONF, (9u << 1) | 1u);
    gpio_write(I2C_CTR, gpio_read(I2C_CTR) | CTR_CONF_UPGATE);

    for (int i = 0; i < 40 && (gpio_read(I2C_SCL_SP_CONF) & 1u); i++)
        udelay(100);

    gpio_write(I2C_SCL_SP_CONF, 0u);
    gpio_write(I2C_CTR, gpio_read(I2C_CTR) | CTR_CONF_UPGATE);
}

static void set_timing(unsigned freq)
{
    /* The module clock is XTAL (40 MHz) when SCLK_SEL is 0. One bit period is
     * (low + high) module cycles, so a half period is sclk / (2 * freq). With no
     * divider that is 200 cycles at 100 kHz, which is what ESP-IDF programs.
     *
     * The previous formula, I2C_SRC_CLK / (freq * 1024) + 1, produced a divider of
     * 391 and a half period of zero, so every timing field wrapped to 0x1FF.
     */
    unsigned clkm_div = I2C_SRC_CLK / (freq * 1024) + 1;
    unsigned sclk = I2C_SRC_CLK / clkm_div;
    unsigned half = sclk / (2 * freq);
    /* Below 80 kHz a long wait-high makes the bus faster than requested; above it,
     * the shorter one is what holds the frequency. */
    unsigned wait_high = (freq >= 80000) ? (half / 2 - 2) : (half / 4);
    unsigned scl_high = half - wait_high;
    /* The SCL timeout is armed on the *level* of SCL: it triggers when SCL stays
     * unchanged for 2 * I2C_TIME_OUT_VALUE module-clock cycles. At 100 kHz with no
     * divider the high period alone is 102 cycles, so ESP-IDF's tabulated value
     * (12) fires in the middle of every bit. The timeout is left disabled; a stuck
     * bus is detected by the transaction never reporting TRANS_COMPLETE.
     */

    /* SCLK_ACTIVE (bit 21) defaults to 1 and gates the module clock: writing the
     * register without it turns the peripheral off and no transaction ever runs.
     */
    gpio_write(I2C_CLK_CONF, (clkm_div - 1) | (1u << 21));            /* SCLK_SEL = 0: XTAL */
    gpio_write(I2C_SCL_LOW_PERIOD, half - 1);
    gpio_write(I2C_SCL_HIGH_PERIOD, scl_high | (wait_high << 9));
    gpio_write(I2C_SDA_HOLD, half / 4 - 1);
    gpio_write(I2C_SDA_SAMPLE, half / 2 - 1);
    gpio_write(I2C_SCL_RSTART_SETUP, half - 1);
    gpio_write(I2C_SCL_STOP_SETUP, half - 1);
    gpio_write(I2C_SCL_START_HOLD, half - 1);
    gpio_write(I2C_SCL_STOP_HOLD, half - 1);
    gpio_write(I2C_TO, 0u);   /* TIME_OUT_EN cleared */
}

void i2c_init(void)
{
    static bool async_ready;


    gpio_write(SYSTEM_PERIP_CLK_EN0, gpio_read(SYSTEM_PERIP_CLK_EN0) | I2C_EXT0_CLK_EN);
    /* ESP-IDF resets the peripheral before configuring it. */
    gpio_write(SYSTEM_PERIP_RST_EN0, gpio_read(SYSTEM_PERIP_RST_EN0) | I2C_EXT0_RST);
    gpio_write(SYSTEM_PERIP_RST_EN0, gpio_read(SYSTEM_PERIP_RST_EN0) & ~I2C_EXT0_RST);

    /* GPIO1 and GPIO2 default to UART0 RX and TX, so the pad function has to be
     * changed before the matrix can use them as plain GPIO. */
    io_mux_select_gpio(I2C_SDA_PIN);
    io_mux_select_gpio(I2C_SCL_PIN);

    /* ESP-IDF idles the lines high and enables the internal pull-ups before
     * connecting the peripheral. The latch matters: if the matrix input samples
     * the output register rather than the pad, a cleared latch reads as a held-low
     * bus and the FSM aborts with arbitration lost.
     */
    gpio_write(GPIO_OUT_W1TS, (1u << I2C_SDA_PIN) | (1u << I2C_SCL_PIN));

    /* TRM 27.4.6: an open-drain master needs I2C_SDA_FORCE_OUT/I2C_SCL_FORCE_OUT
     * set *and* the pad driver set to open drain. Without the pad driver the FSM
     * drives SDA push-pull, the slave cannot pull it low during the acknowledge
     * slot, and every transaction with ack_check_en set reports NACK. This is the
     * single bit that separates a working bus from one that never ACKs.
     */
    gpio_write(GPIO_PIN_REG(I2C_SDA_PIN),
               gpio_read(GPIO_PIN_REG(I2C_SDA_PIN)) | GPIO_PIN_PAD_DRIVER);
    gpio_write(GPIO_PIN_REG(I2C_SCL_PIN),
               gpio_read(GPIO_PIN_REG(I2C_SCL_PIN)) | GPIO_PIN_PAD_DRIVER);
    gpio_write(IO_MUX_PIN(I2C_SDA_PIN), gpio_read(IO_MUX_PIN(I2C_SDA_PIN)) | (1u << 8));
    gpio_write(IO_MUX_PIN(I2C_SCL_PIN), gpio_read(IO_MUX_PIN(I2C_SCL_PIN)) | (1u << 8));

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
    gpio_write(I2C_FILTER_CFG, 7u | (7u << 4) | (1u << 8) | (1u << 9));

    /* Direct (non-FIFO) access to TX and RX RAM: each byte occupies a whole word at a
     * fixed address, which is simpler than the auto-advancing pointers and is what
     * double addressing requires anyway. */
    /* FIFO mode: RX watermark 11, TX watermark 5, FIFO protection on. This is
     * what ESP-IDF's HAL programs; direct (non-FIFO) addressing is only needed
     * for slave-mode double addressing.
     */
    gpio_write(I2C_FIFO_CONF, 11u | (5u << 5) | (1u << 14));

    /* The transaction is polled to completion, so the controller's interrupt stays
     * masked. An unmasked TRANS_COMPLETE would enter i2c_irq() with no transaction
     * in flight.
     */
    if (!async_ready) {
        __i2c_async_init();
        async_ready = true;
    }
    /* The transaction is polled inside __i2c_async_submit, so the controller's own
     * interrupt is deliberately left masked: an unmasked TRANS_COMPLETE would enter
     * the level-1 handler with kernel work in flight on the interrupted thread.
     */
    gpio_write(I2C_INT_ENA, 0u);
    /* Same master configuration as start_list uses, so the peripheral is in master
     * mode with arbitration off before the first transaction. */
    gpio_write(I2C_CTR, CTR_SDA_FORCE_OUT | CTR_SCL_FORCE_OUT | CTR_MS_MODE);

    /* Recovery polls rather than interrupts, and a stale TRANS_COMPLETE bit would
     * otherwise complete a transaction that never started. */
    i2c_bus_recovery();
    gpio_write(I2C_INT_CLR, 0xFFFF);
}

/* The single path from the synchronous API to the hardware. */
static void sync_callback(int status, i2c_descriptor *d)
{
    (void)d;
    cb_status = status;
    cb_done = 1;
}

/* The synchronous API drives the async queue and then polls for the completion
 * callback rather than blocking on a semaphore: the callback runs on the
 * interrupted thread's stack, so blocking the caller would deadlock the very
 * context the handler is running in.
 */
static int sync_run(i2c_descriptor *desc)
{
    desc->callback = sync_callback;
    desc->arg = 0;
    desc->next = NULL;
    cb_status = I2C_STATUS_ERROR;
    cb_done = 0;

    if (i2c_async_queue(0, TIMEOUT_NOBLOCK, I2C_Q_ADD, 0, desc) != I2C_RC_OK) {
        return I2C_STATUS_ERROR;
    }

    /* __i2c_async_submit runs the transaction to completion before returning, so
     * the callback has already fired by the time the queue call does.
     */
    return cb_done ? cb_status : I2C_STATUS_ERROR;
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
