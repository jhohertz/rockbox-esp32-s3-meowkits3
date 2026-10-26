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

#include <stdio.h>   /* the ledc_* failure reports below need it */

#include "driver/ledc.h"
#include "esp_err.h"



#define BL_PIN        42
#define BL_CHANNEL    7
#define BL_TIMER      0

#define BL_RES_BITS   9
#define BL_FULL_DUTY  (1u << BL_RES_BITS)   /* 512: the counter never reaches it */

/* 5 kHz matches the reference firmware's default and is far above anything the
 * eye can resolve on a display backlight. */
#define BL_FREQ_HZ    5000

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)reg; }
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


    return true;
}

void backlight_hw_on(void)
{
    /* Wake the panel before light is applied, otherwise the backlight comes up
     * onto a panel that is still coming out of sleep and the first frame is lost.
     */
    lcd_enable(true);

#if (CONFIG_BACKLIGHT_FADING != BACKLIGHT_FADING_SW_SETTING)
    /* Without software fading, backlight_update_state() calls hw_on() and nothing
     * else: there is no follow-up backlight_hw_brightness() on that configuration
     * (backlight.c:527-531), so hw_on alone has to produce the right level. Asking
     * for MAX_BRIGHTNESS_SETTING instead made every off->on transition flash to
     * full before the queued BACKLIGHT_BRIGHTNESS_CHANGED landed -- visible as a
     * flicker while adjusting the setting.
     *
     * With software fading the core ramps instead: backlight_setup_fade_up()
     * calls hw_on() to get the lamp lit at whatever the hardware currently holds,
     * then steps up through backlight_hw_brightness() one BRIGHTNESS_STEP at a
     * time (backlight-sw-fading.c:49-62). Setting the target level here would
     * defeat that, which is why backlight-e200v2-fuze.c:40-44 guards the same
     * line the same way and says so: "if we set the brightness to the settings
     * value, then fading up is glitchy". */
    backlight_hw_brightness(backlight_brightness);
#endif
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

    /* Scale 0..MAX to 0..512 with rounding. The general form keeps it correct if
     * the target changes MAX_BRIGHTNESS_SETTING. */
    unsigned duty = (unsigned)((brightness * BL_FULL_DUTY + MAX_BRIGHTNESS_SETTING / 2)
                               / MAX_BRIGHTNESS_SETTING);

    set_duty(duty);
}
