/***************************************************************************
 * MeowKit ESP32-S3 backlight.
 *
 * The panel's backlight is driven from GPIO 42 and is active low: the LED is on
 * when the line is low. The LEDC peripheral can PWM that pin, so brightness is a
 * duty cycle rather than a binary switch.
 *
 * LEDC does not drive the pad itself - the signal has to be routed to the pin
 * through the GPIO matrix, which is why the pin is not simply written.
 *
 * ESP32-S3 has eight low-speed channels and four low-speed timers only; there is
 * no high-speed group. The timer counts 0..hpoint-1 and the signal is high while
 * the counter is below duty, so with a 9-bit resolution and hpoint 512 a duty of
 * 512 is "always high", which for an active-low LED means off. That is the same
 * convention the reference firmware uses (ledcWrite(7, 512) = off).
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "lcd.h"
#include "backlight-target.h"
#include "xtensa.h"
#include "bootlog.h"
#include "gpio-xtensa.h"

/* A low-speed channel is five words wide - conf0, hpoint, duty, conf1,
 * duty_r - so the stride is 0x14, not 0x10: LSCH7_CONF0 sits at base + 0x8c.
 * A timer is two words (conf, value), so the timer stride is 0x8.
 * See ESP-IDF components/soc/esp32s3/register/soc/ledc_reg.h. */
#define LEDC_CONF             (XT_LEDC_BASE + 0xd0)
#define LEDC_LSCH_CONF0(ch)   (XT_LEDC_BASE + 0x00 + 0x14 * (ch))
#define LEDC_LSCH_HPOINT(ch)  (XT_LEDC_BASE + 0x04 + 0x14 * (ch))
#define LEDC_LSCH_DUTY(ch)    (XT_LEDC_BASE + 0x08 + 0x14 * (ch))
#define LEDC_LSTIMER_CONF(t)  (XT_LEDC_BASE + 0xa0 + 0x08 * (t))

/* apb_clk_sel bits [1:0] of LEDC_CONF: 1 = APB, 2 = RC_FAST, 3 = XTAL. */
#define LEDC_CLK_XTAL         3u

#define BL_PIN        42
#define BL_CHANNEL    7
#define BL_TIMER      0
#define BL_SIG_OUT    80        /* LEDC_LS_SIG_OUT7_IDX */

#define BL_RES_BITS   9
#define BL_FULL_DUTY  (1u << BL_RES_BITS)   /* 512: the counter never reaches it */

/* XTAL is the dependable clock here. The PLL is not guaranteed to be up when
 * Rockbox is entered from the bootloader, and a backlight does not need it.
 * 40 MHz / (512 * 8) = 9766 Hz. */
#define BL_CLK_DIV    8

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

static void set_duty(unsigned duty)
{
    /* Inverted: the LED lights when the line is low, so a small duty is bright. */
    wr(LEDC_LSCH_DUTY(BL_CHANNEL), BL_FULL_DUTY - duty);
}

bool backlight_hw_init(void)
{
    gpio_matrix_out(BL_PIN, BL_SIG_OUT);
    gpio_set_direction(BL_PIN, 1);

    /* The low-speed timer clock source is a global field, not a per-timer one. */
    wr(LEDC_CONF, (rd(LEDC_CONF) & ~3u) | LEDC_CLK_XTAL);

    /* duty_res [3:0] is the number of resolution bits, clk_div [21:4],
     * pause [22], rst [23], tick_sel [24]. rst defaults to 1 and is a level, not
     * a self-clearing bit: leaving it set holds the counter in reset and the
     * channel outputs the idle level forever, so it has to be pulsed. */
    uint32_t conf = BL_RES_BITS | ((uint32_t)BL_CLK_DIV << 4);
    wr(LEDC_LSTIMER_CONF(BL_TIMER), conf | (1u << 23));
    wr(LEDC_LSTIMER_CONF(BL_TIMER), conf);

    /* The period is hpoint, and its reset value is zero. Without it the counter
     * has no wrap point and the duty comparison is meaningless. */
    wr(LEDC_LSCH_HPOINT(BL_CHANNEL), BL_FULL_DUTY);

    /* Channel: timer_sel [1:0], sig_out_en [2]; idle_lv [3] stays at its reset 0. */
    wr(LEDC_LSCH_CONF0(BL_CHANNEL), (BL_TIMER & 3u) | (1u << 2));

    set_duty(0);
    return true;
}

void backlight_hw_on(void)
{
    boot_trace('k');

    /* Wake the panel before light is applied, otherwise the backlight comes up
     * onto a panel that is still coming out of sleep and the first frame is lost.
     */
    lcd_enable(true);
    backlight_hw_brightness(MAX_BRIGHTNESS_SETTING);
}

void backlight_hw_off(void)
{
    set_duty(0);
    lcd_enable(false);
}

void backlight_hw_brightness(int brightness)
{
    if (brightness < 0)
        brightness = 0;
    if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;

    /* Scale 0..MAX to 0..511. MAX_BRIGHTNESS_SETTING is 255, so this is a shift
     * with rounding; the general form keeps it correct if the target changes it. */
    unsigned duty = (unsigned)((brightness * BL_FULL_DUTY + MAX_BRIGHTNESS_SETTING / 2)
                               / MAX_BRIGHTNESS_SETTING);

    set_duty(duty);
}
