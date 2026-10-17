/***************************************************************************
 * MeowKit ESP32-S3 backlight.
 *
 * The panel's backlight is driven from GPIO 42 and is active low: the LED is on
 * when the line is low. LEDC PWMs that pin, so brightness is a duty cycle rather
 * than a binary switch.
 *
 * This goes through IDF's LEDC driver (ledc_timer_config / ledc_channel_config /
 * ledc_set_duty / ledc_update_duty) rather than poking the registers directly.
 * That is a deliberate choice, not a convenience: the hand-rolled version of this
 * file wrote the correct addresses with the correct bit fields -- every offset and
 * mask was checked against esp32s3's ledc_reg.h and ledc_struct.h -- and not one
 * write ever reached the register file. LEDC_CONF read back 0 immediately after
 * BIT(31) was written to it, and every other LEDC register read back exactly its
 * reset value, while the SYSTEM gate (PERIP_CLK_EN0 bit 11) was confirmed set and
 * staying set. The first version of this file also omitted the
 * conf0.low_speed_update latch that ledc_update_duty() performs, so it was wrong
 * twice over. IDF owns the gate, the slow-clock selection, and the latch.
 *
 * LEDC does not drive the pad itself - the signal is routed to the pin through the
 * GPIO matrix, which ledc_channel_config() arranges; the pin is not simply
 * written.
 *
 * ESP32-S3 has eight low-speed channels and four low-speed timers only; there is
 * no high-speed group. The timer counts 0..hpoint-1 and the signal is high while
 * the counter is below duty, so at 9-bit resolution a duty of 512 is "always
 * high", which for an active-low LED means off. That is the same convention the
 * reference firmware uses (ledcWrite(7, 512) = off), and the same reason
 * backlight_hw_brightness() inverts before handing the value over.
 ****************************************************************************/
#include "config.h"

#include "system.h"
#include "lcd.h"
#include "backlight.h"        /* backlight_brightness: the level to restore on */
#include "backlight-target.h"
#include "xtensa.h"

#include <stdio.h>   /* TEMPORARY: only for bl_probe's printf */

#include "driver/ledc.h"
#include "esp_err.h"

/* Only the three addresses bl_probe() reads. gpio-xtensa.h is deliberately NOT
 * included here: it declares gpio_set_direction(int, int) as a static inline, and
 * driver/gpio.h -- pulled in by driver/ledc.h -- declares IDF's
 * gpio_set_direction(gpio_num_t, gpio_mode_t). The two cannot coexist in one
 * translation unit. Nothing here needs the helpers anyway now that
 * ledc_channel_config() arranges the GPIO matrix routing. */
#define DR_REG_GPIO_BASE     0x60004000
#define DR_REG_IO_MUX_BASE   0x60009000
#define IO_MUX_PIN(n)        (DR_REG_IO_MUX_BASE + 0x04 + 4u * (n))
#define GPIO_FUNC_OUT_SEL(n) (DR_REG_GPIO_BASE + 0x554 + 4u * (n))

/* Kept only so bl_probe() can read the hardware back and show that IDF's writes
 * land where ours did not. A low-speed channel is five words wide - conf0,
 * hpoint, duty, conf1, duty_r - so the stride is 0x14, not 0x10: LSCH7_CONF0 sits
 * at base + 0x8c and its duty read-back at base + 0x9c. A low-speed timer is two
 * words (conf, value), so the timer stride is 0x8.
 * See ESP-IDF components/soc/esp32s3/register/soc/ledc_reg.h. */
#define LEDC_CONF             (XT_LEDC_BASE + 0xd0)
#define LEDC_LSCH_CONF0(ch)   (XT_LEDC_BASE + 0x00 + 0x14 * (ch))
#define LEDC_LSCH_HPOINT(ch)  (XT_LEDC_BASE + 0x04 + 0x14 * (ch))
#define LEDC_LSCH_DUTY(ch)    (XT_LEDC_BASE + 0x08 + 0x14 * (ch))
#define LEDC_LSCH_CONF1(ch)   (XT_LEDC_BASE + 0x0c + 0x14 * (ch))
#define LEDC_LSCH_DUTY_R(ch)  (XT_LEDC_BASE + 0x10 + 0x14 * (ch))
#define LEDC_LSTIMER_CONF(t)  (XT_LEDC_BASE + 0xa0 + 0x08 * (t))

#define BL_PIN        42
#define BL_CHANNEL    7
#define BL_TIMER      0

#define BL_RES_BITS   9
#define BL_FULL_DUTY  (1u << BL_RES_BITS)   /* 512: the counter never reaches it */

/* 5 kHz matches the reference firmware's default and is far above anything the
 * eye can resolve on a display backlight. */
#define BL_FREQ_HZ    5000

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
static inline void wr(uint32_t reg, uint32_t val) { *(volatile uint32_t *)reg = val; }

/* TEMPORARY: read the whole path back rather than infer it. Written duty vs
 * duty_r separates "the write did not land" from "the write landed but the pin
 * does not follow", and the pad/matrix columns cover the third possibility --
 * that the signal is correct and simply not routed to GPIO 42.
 *
 * Under the hand-rolled driver every line here reported the reset value. It is
 * retained so that difference is visible in the log rather than taken on trust. */
static void bl_probe(const char *where)
{
    printf("BL %s: conf=%08lx tconf=%08lx chconf=%08lx hpt=%lu duty=%lu "
           "conf1=%08lx RD=%lu\n",
           where,
           (unsigned long)rd(LEDC_CONF),
           (unsigned long)rd(LEDC_LSTIMER_CONF(BL_TIMER)),
           (unsigned long)rd(LEDC_LSCH_CONF0(BL_CHANNEL)),
           (unsigned long)(rd(LEDC_LSCH_HPOINT(BL_CHANNEL)) & 0x3fffu),
           (unsigned long)(rd(LEDC_LSCH_DUTY(BL_CHANNEL)) & 0x7ffffu),
           (unsigned long)rd(LEDC_LSCH_CONF1(BL_CHANNEL)),
           (unsigned long)(rd(LEDC_LSCH_DUTY_R(BL_CHANNEL)) & 0x7ffffu));
    printf("BL   pad: iomux=%04lx out_sel=%03lx inv=%d gpio_out_bit42=%d\n",
           (unsigned long)(rd(IO_MUX_PIN(BL_PIN)) & 0xffffu),
           (unsigned long)(rd(GPIO_FUNC_OUT_SEL(BL_PIN)) & 0x1ffu),
           (rd(GPIO_FUNC_OUT_SEL(BL_PIN)) >> 31) & 1u,
           (rd(DR_REG_GPIO_BASE + 0x04) >> BL_PIN) & 1u);
    printf("BL   gate: perip_clk_en0=%08lx bit11=%d\n",
           (unsigned long)rd(XT_PERIP_CLK_EN0),
           (rd(XT_PERIP_CLK_EN0) >> 11) & 1u);
}

/* Hand a Rockbox brightness (0 = off .. MAX_BRIGHTNESS_SETTING = full) to the
 * hardware. Inverted because the LED lights on a low: Rockbox's maximum is the
 * smallest duty, and Rockbox's zero is a duty the counter can never reach. */
static void set_duty(unsigned duty)
{
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, BL_FULL_DUTY - duty) != ESP_OK)
        printf("BL ledc_set_duty failed\n");
    /* This call is the latch. The duty and hpoint registers sit in the APB
     * domain and the channel only samples them into its own clock domain when
     * conf0.low_speed_update is pulsed, which is what ledc_update_duty() does. */
    if (ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7) != ESP_OK)
        printf("BL ledc_update_duty failed\n");
}

bool backlight_hw_init(void)
{
    printf("BL gate before init: perip_clk_en0=%08lx bit11=%d\n",
           (unsigned long)rd(XT_PERIP_CLK_EN0),
           (rd(XT_PERIP_CLK_EN0) >> 11) & 1u);

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = (ledc_timer_t)BL_TIMER,
        .duty_resolution = LEDC_TIMER_9_BIT,
        .freq_hz         = BL_FREQ_HZ,
        /* Let IDF pick the slow clock. The hand-rolled version pinned XTAL on
         * the theory that the PLL is not guaranteed up when Rockbox is entered
         * from the bootloader; IDF's choice is measured against sdkconfig and
         * it calibrates RC_FAST when it picks that, so the judgement is better
         * placed there than here. */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK)
    {
        printf("BL ledc_timer_config failed\n");
        return false;
    }

    ledc_channel_config_t chan = {
        .gpio_num   = BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = (ledc_channel_t)BL_CHANNEL,
        .timer_sel  = (ledc_timer_t)BL_TIMER,
        /* Start dark: full duty holds the line high, and the LED is off on a
         * high. Rockbox turns the backlight on via backlight_hw_on(). */
        .duty       = BL_FULL_DUTY,
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };
    if (ledc_channel_config(&chan) != ESP_OK)
    {
        printf("BL ledc_channel_config failed\n");
        return false;
    }

    bl_probe("at end of init");

    return true;
}

void backlight_hw_on(void)
{
    /* Wake the panel before light is applied, otherwise the backlight comes up
     * onto a panel that is still coming out of sleep and the first frame is lost.
     */
    lcd_enable(true);

    /* Restore the level Rockbox is asking for, not maximum. With
     * CONFIG_BACKLIGHT_FADING == BACKLIGHT_NO_FADING -- which is what this target
     * gets, since it does not define one -- backlight_update_state() calls
     * backlight_hw_on() and nothing else: there is no follow-up
     * backlight_hw_brightness() on this configuration. backlight_setup_fade_up()
     * at backlight.c:456 shows the pattern the fading targets rely on. So on this
     * target hw_on alone has to produce the right level, and asking for
     * MAX_BRIGHTNESS_SETTING made every off->on transition flash to full before
     * the queued BACKLIGHT_BRIGHTNESS_CHANGED landed -- visible as a flicker while
     * adjusting the setting. */
    backlight_hw_brightness(backlight_brightness);
    bl_probe("at hw_on   ");
}

void backlight_hw_off(void)
{
    set_duty(0);
    bl_probe("at hw_off  ");
    lcd_enable(false);
}

void backlight_hw_brightness(int brightness)
{
    if (brightness < 0)
        brightness = 0;
    if (brightness > MAX_BRIGHTNESS_SETTING)
        brightness = MAX_BRIGHTNESS_SETTING;

    /* Scale 0..MAX to 0..512 with rounding. The general form keeps it correct if
     * the target changes MAX_BRIGHTNESS_SETTING. */
    unsigned duty = (unsigned)((brightness * BL_FULL_DUTY + MAX_BRIGHTNESS_SETTING / 2)
                               / MAX_BRIGHTNESS_SETTING);

    set_duty(duty);
}
