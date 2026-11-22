/*
 * hardware.c - the beginner built-ins on a real ESP32:
 *
 *   turn_on(pin), turn_off(pin), toggle(pin)   any output
 *   brightness(pin, percent)                   PWM, for LEDs and motors
 *   color(pin, c), color_at(pin, i, c)         WS2812 ("NeoPixel") LEDs
 *   tone(pin, hz_or_note, ms), beep(pin)       buzzers and speakers
 *   servo(pin, angle)                          hobby servos, 0..180
 *   pressed(pin)                               a button to ground
 *   analog(pin)                                0..4095 from the ADC
 *   distance(trig, echo)                       HC-SR04 sensor, in cm
 *   led(x, y, c), led_clear(), led_show(),     a WS2812 LED panel, set up
 *   led_text(msg, x, y, c), led_width(),       with the PANEL_ settings below
 *   led_height(), led_size()
 *
 * PWM uses the LEDC peripheral: timer 0 for brightness, timer 1 for
 * servos and timer 2 for tones. WS2812 LEDs use the RMT peripheral.
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hardware.h"
#include "hw.h"
#include "led_strip.h"

#define MAX_PWM 6      /* LEDC channels used, the minimum every ESP32 has */

/* The LED panel. The common 8x32 WS2812 panels are wired in a zigzag: down
   the first column, up the next, and so on. Set PANEL_ZIGZAG to 0 for a
   panel wired row by row, left to right. */
#define PANEL_PIN 2       /* the panel's DIN; GPIO 2 is D1 on a XIAO ESP32-S3 */
#define PANEL_W 32
#define PANEL_H 8
#define PANEL_ZIGZAG 1
/* Each LED at full white draws about 60 mA, so a whole panel would need more
   than USB can give. Colours are scaled down to this many 255ths. */
#define PANEL_BRIGHTNESS 24
#define MAX_STRIPS 2
#define STRIP_LEDS 64

static int8_t level[GPIO_NUM_MAX];
static int8_t button_was[GPIO_NUM_MAX]; /* 0 unknown, 1 up, 2 down */
static int64_t button_changed[GPIO_NUM_MAX];

static bool get_pin(JlVM *vm, const char *fn, int arg_no, Value v, int32_t *pin) {
    if (!jl_get_int(vm, fn, arg_no, v, pin)) return false;
    if (!GPIO_IS_VALID_GPIO(*pin)) return jl_error(vm, "%s(): GPIO %d doesn't exist on this chip", fn, (int)*pin);
    return true;
}

static bool get_out_pin(JlVM *vm, const char *fn, int arg_no, Value v, int32_t *pin) {
    if (!get_pin(vm, fn, arg_no, v, pin)) return false;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(*pin)) return jl_error(vm, "%s(): GPIO %d can only be an input", fn, (int)*pin);
    return true;
}

/* ---- PWM ------------------------------------------------------------------- */

static struct {
    int pin;
    ledc_timer_t timer;
} pwm[MAX_PWM]; /* pin -1 = unused, set in hardware_register() */
static bool timer_ready[LEDC_TIMER_MAX];
static ledc_timer_bit_t timer_bits[LEDC_TIMER_MAX];

static bool timer_setup(JlVM *vm, const char *fn, ledc_timer_t t, uint32_t hz) {
    ledc_timer_bit_t bits = (ledc_timer_bit_t)ledc_find_suitable_duty_resolution(80000000, hz);
    if (bits > LEDC_TIMER_14_BIT) bits = LEDC_TIMER_14_BIT;
    ledc_timer_config_t cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = bits,
        .timer_num = t,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&cfg) != ESP_OK) return jl_error(vm, "%s(): the chip can't make %u Hz", fn, (unsigned)hz);
    timer_ready[t] = true;
    timer_bits[t] = bits;
    return true;
}

/* The LEDC channel driving `pin` from timer t, set up on first use. */
static bool channel_for(JlVM *vm, const char *fn, int pin, ledc_timer_t t, ledc_channel_t *ch) {
    int free_slot = -1;
    for (int i = 0; i < MAX_PWM; i++) {
        if (pwm[i].pin == pin && pwm[i].timer == t) {
            *ch = (ledc_channel_t)i;
            return true;
        }
        if (pwm[i].pin == pin || (pwm[i].pin < 0 && free_slot < 0)) free_slot = i;
    }
    if (free_slot < 0) return jl_error(vm, "%s(): at most %d pins can use brightness, servo and tone at once", fn, MAX_PWM);
    ledc_channel_config_t cfg = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)free_slot,
        .timer_sel = t,
        .duty = 0,
    };
    if (ledc_channel_config(&cfg) != ESP_OK) return jl_error(vm, "%s(): GPIO %d can't do PWM", fn, pin);
    pwm[free_slot].pin = pin;
    pwm[free_slot].timer = t;
    *ch = (ledc_channel_t)free_slot;
    return true;
}

static void set_duty(ledc_channel_t ch, uint32_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

static bool b_brightness(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, pct;
    ledc_channel_t ch;
    if (!get_out_pin(vm, "brightness", 1, args[0], &pin) || !jl_get_int(vm, "brightness", 2, args[1], &pct)) return false;
    if (pct < 0 || pct > 100) return jl_error(vm, "brightness(): use a percentage from 0 to 100, got %d", (int)pct);
    if (!timer_ready[LEDC_TIMER_0] && !timer_setup(vm, "brightness", LEDC_TIMER_0, 5000)) return false;
    if (!channel_for(vm, "brightness", pin, LEDC_TIMER_0, &ch)) return false;
    /* Eyes see brightness on a curve, so square the percentage. */
    uint32_t max = (1u << timer_bits[LEDC_TIMER_0]) - 1;
    set_duty(ch, (uint32_t)((uint64_t)max * (uint32_t)(pct * pct) / 10000));
    return true;
}

static bool b_servo(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, angle;
    ledc_channel_t ch;
    if (!get_out_pin(vm, "servo", 1, args[0], &pin) || !jl_get_int(vm, "servo", 2, args[1], &angle)) return false;
    if (angle < 0 || angle > 180) return jl_error(vm, "servo(): the angle must be 0 to 180 degrees, got %d", (int)angle);
    if (!timer_ready[LEDC_TIMER_1] && !timer_setup(vm, "servo", LEDC_TIMER_1, 50)) return false;
    if (!channel_for(vm, "servo", pin, LEDC_TIMER_1, &ch)) return false;
    /* 0.5 ms to 2.5 ms pulses every 20 ms */
    uint32_t us = 500 + (uint32_t)angle * 2000 / 180;
    set_duty(ch, (uint32_t)((uint64_t)us * (1u << timer_bits[LEDC_TIMER_1]) / 20000));
    return true;
}

static bool play(JlVM *vm, const char *fn, int32_t pin, Value note, int32_t ms) {
    int32_t hz;
    ledc_channel_t ch;
    if (!jl_hw_note(vm, fn, 2, note, &hz)) return false;
    if (ms < 0) return jl_error(vm, "%s(): can't play for a negative time (%d ms)", fn, (int)ms);
    if (hz > 0) {
        if (!timer_setup(vm, fn, LEDC_TIMER_2, (uint32_t)hz)) return false;
        if (!channel_for(vm, fn, pin, LEDC_TIMER_2, &ch)) return false;
        set_duty(ch, 1u << (timer_bits[LEDC_TIMER_2] - 1)); /* a square wave */
    }
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
    if (hz > 0) set_duty(ch, 0);
    return true;
}

static bool b_tone(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, ms;
    if (!get_out_pin(vm, "tone", 1, args[0], &pin) || !jl_get_int(vm, "tone", 3, args[2], &ms)) return false;
    return play(vm, "tone", pin, args[1], ms);
}

static bool b_beep(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    if (!get_out_pin(vm, "beep", 1, args[0], &pin)) return false;
    return play(vm, "beep", pin, int_val(1000), 100);
}

/* ---- plain outputs ----------------------------------------------------------- */

static void release_pwm(int pin) {
    for (int i = 0; i < MAX_PWM; i++) {
        if (pwm[i].pin == pin) {
            ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, 0);
            pwm[i].pin = -1;
        }
    }
}

static bool set_level(JlVM *vm, const char *fn, Value v, int which) {
    int32_t pin;
    if (!get_out_pin(vm, fn, 1, v, &pin)) return false;
    release_pwm(pin);
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    level[pin] = (int8_t)(which < 0 ? !level[pin] : which);
    gpio_set_level((gpio_num_t)pin, (uint32_t)level[pin]);
    return true;
}

static bool b_turn_on(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    return set_level(vm, "turn_on", args[0], 1);
}

static bool b_turn_off(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    return set_level(vm, "turn_off", args[0], 0);
}

static bool b_toggle(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    return set_level(vm, "toggle", args[0], -1);
}

/* ---- WS2812 colour LEDs --------------------------------------------------------- */

static struct {
    int pin;
    led_strip_handle_t strip;
} strips[MAX_STRIPS]; /* pin -1 = unused */

static bool strip_for(JlVM *vm, const char *fn, int pin, led_strip_handle_t *out) {
    for (int i = 0; i < MAX_STRIPS; i++)
        if (strips[i].pin == pin) return *out = strips[i].strip, true;
    for (int i = 0; i < MAX_STRIPS; i++) {
        if (strips[i].pin >= 0) continue;
        led_strip_config_t cfg = {
            .strip_gpio_num = pin,
            .max_leds = STRIP_LEDS,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
        if (led_strip_new_rmt_device(&cfg, &rmt, &strips[i].strip) != ESP_OK)
            return jl_error(vm, "%s(): GPIO %d can't drive colour LEDs", fn, pin);
        strips[i].pin = pin;
        led_strip_clear(strips[i].strip);
        *out = strips[i].strip;
        return true;
    }
    return jl_error(vm, "%s(): colour LEDs can be on at most %d pins", fn, MAX_STRIPS);
}

static bool show(JlVM *vm, const char *fn, int32_t pin, int32_t index, uint32_t rgb) {
    led_strip_handle_t strip;
    if (!strip_for(vm, fn, pin, &strip)) return false;
    led_strip_set_pixel(strip, (uint32_t)index, rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF);
    led_strip_refresh(strip);
    return true;
}

static bool b_color(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    uint32_t rgb;
    if (!get_out_pin(vm, "color", 1, args[0], &pin) || !jl_hw_color(vm, "color", 2, args[1], &rgb)) return false;
    return show(vm, "color", pin, 0, rgb);
}

static bool b_color_at(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, index;
    uint32_t rgb;
    if (!get_out_pin(vm, "color_at", 1, args[0], &pin) || !jl_get_int(vm, "color_at", 2, args[1], &index) ||
        !jl_hw_color(vm, "color_at", 3, args[2], &rgb))
        return false;
    if (index < 0 || index >= STRIP_LEDS)
        return jl_error(vm, "color_at(): LED number must be 0..%d, got %d", STRIP_LEDS - 1, (int)index);
    return show(vm, "color_at", pin, index, rgb);
}

/* ---- inputs ------------------------------------------------------------------- */

static bool b_pressed(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "pressed", 1, args[0], &pin)) return false;
    if (button_was[pin] == 0) {
        gpio_reset_pin((gpio_num_t)pin);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
        esp_rom_delay_us(50);
        button_was[pin] = 1;
    }
    int down = gpio_get_level((gpio_num_t)pin) == 0;
    int64_t now = esp_timer_get_time();
    bool fired = false;
    /* Ignore changes within 30 ms of the last one: contacts bounce. */
    if ((down ? 2 : 1) != button_was[pin] && now - button_changed[pin] > 30000) {
        fired = down;
        button_was[pin] = down ? 2 : 1;
        button_changed[pin] = now;
    }
    *out = bool_val(fired);
    return true;
}

static adc_oneshot_unit_handle_t adc_units[2];

static bool b_analog(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    adc_unit_t unit;
    adc_channel_t ch;
    if (!get_pin(vm, "analog", 1, args[0], &pin)) return false;
    if (adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK)
        return jl_error(vm, "analog(): GPIO %d can't read analog values on this chip", (int)pin);
    if (adc_units[unit] == NULL) {
        adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
        if (adc_oneshot_new_unit(&ucfg, &adc_units[unit]) != ESP_OK)
            return jl_error(vm, "analog(): GPIO %d's ADC is busy (on some chips Wi-Fi uses it)", (int)pin);
    }
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    adc_oneshot_config_channel(adc_units[unit], ch, &ccfg);
    int raw = 0;
    if (adc_oneshot_read(adc_units[unit], ch, &raw) != ESP_OK)
        return jl_error(vm, "analog(): reading GPIO %d failed", (int)pin);
    *out = int_val(raw);
    return true;
}

/* HC-SR04: a 10 us pulse on trig, then echo stays high for 58 us per cm. */
static bool b_distance(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t trig, echo;
    if (!get_out_pin(vm, "distance", 1, args[0], &trig) || !get_pin(vm, "distance", 2, args[1], &echo)) return false;
    gpio_set_direction((gpio_num_t)trig, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)echo, GPIO_MODE_INPUT);
    gpio_set_level((gpio_num_t)trig, 0);
    esp_rom_delay_us(4);
    gpio_set_level((gpio_num_t)trig, 1);
    esp_rom_delay_us(10);
    gpio_set_level((gpio_num_t)trig, 0);
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)echo) == 0)
        if (esp_timer_get_time() - start > 30000) return *out = int_val(-1), true;
    int64_t rise = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)echo) == 1)
        if (esp_timer_get_time() - rise > 30000) return *out = int_val(-1), true;
    *out = int_val((int32_t)((esp_timer_get_time() - rise) / 58));
    return true;
}

/* ---- the LED panel -------------------------------------------------------------- */

static uint32_t panel[PANEL_W * PANEL_H];
static led_strip_handle_t panel_strip;

static void panel_set(int x, int y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < PANEL_W && y < PANEL_H) panel[y * PANEL_W + x] = rgb;
}

static bool b_led(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t x, y;
    uint32_t rgb;
    if (!jl_get_int(vm, "led", 1, args[0], &x) || !jl_get_int(vm, "led", 2, args[1], &y) ||
        !jl_hw_color(vm, "led", 3, args[2], &rgb))
        return false;
    panel_set(x, y, rgb);
    return true;
}

static bool b_led_clear(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args, (void)out;
    memset(panel, 0, sizeof panel);
    return true;
}

static bool b_led_show(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args, (void)out;
    if (panel_strip == NULL) {
        led_strip_config_t cfg = {
            .strip_gpio_num = PANEL_PIN,
            .max_leds = PANEL_W * PANEL_H,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
        if (led_strip_new_rmt_device(&cfg, &rmt, &panel_strip) != ESP_OK)
            return jl_error(vm, "led_show(): couldn't start the LED panel on GPIO %d", PANEL_PIN);
    }
    for (int x = 0; x < PANEL_W; x++) {
        for (int y = 0; y < PANEL_H; y++) {
            uint32_t c = panel[y * PANEL_W + x];
            int i = PANEL_ZIGZAG ? x * PANEL_H + (x % 2 ? PANEL_H - 1 - y : y) : y * PANEL_W + x;
            led_strip_set_pixel(panel_strip, (uint32_t)i, (c >> 16 & 0xFF) * PANEL_BRIGHTNESS / 255,
                                (c >> 8 & 0xFF) * PANEL_BRIGHTNESS / 255, (c & 0xFF) * PANEL_BRIGHTNESS / 255);
        }
    }
    led_strip_refresh(panel_strip);
    return true;
}

static bool b_led_width(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(PANEL_W);
    return true;
}

static bool b_led_height(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(PANEL_H);
    return true;
}

static bool b_led_size(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args;
    Value *items;
    if (!jl_new_array(vm, 2, out, &items)) return false;
    items[0] = int_val(PANEL_W);
    items[1] = int_val(PANEL_H);
    return true;
}

static uint32_t text_rgb;
static void plot(void *ud, int x, int y) {
    (void)ud;
    panel_set(x, y, text_rgb);
}

static bool b_led_text(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t x, y;
    const char *s;
    uint32_t n;
    char num[16];
    if (args[0].type == VAL_INT) {
        n = (uint32_t)snprintf(num, sizeof num, "%d", (int)args[0].as.i);
        s = num;
    } else if (args[0].type != VAL_STR || !jl_get_string(vm, args[0], &s, &n)) {
        return jl_error(vm, "led_text(): the text must be a string or a number, got %s", value_type_name(args[0]));
    }
    if (!jl_get_int(vm, "led_text", 2, args[1], &x) || !jl_get_int(vm, "led_text", 3, args[2], &y) ||
        !jl_hw_color(vm, "led_text", 4, args[3], &text_rgb))
        return false;
    *out = int_val(jl_hw_text(s, n, x, y, plot, NULL));
    return true;
}

void hardware_register(JlVM *vm) {
    for (int i = 0; i < MAX_PWM; i++) pwm[i].pin = -1;
    for (int i = 0; i < MAX_STRIPS; i++) strips[i].pin = -1;
    jl_register_native(vm, "turn_on", b_turn_on, 1);
    jl_register_native(vm, "turn_off", b_turn_off, 1);
    jl_register_native(vm, "toggle", b_toggle, 1);
    jl_register_native(vm, "brightness", b_brightness, 2);
    jl_register_native(vm, "color", b_color, 2);
    jl_register_native(vm, "color_at", b_color_at, 3);
    jl_register_native(vm, "tone", b_tone, 3);
    jl_register_native(vm, "beep", b_beep, 1);
    jl_register_native(vm, "servo", b_servo, 2);
    jl_register_native(vm, "pressed", b_pressed, 1);
    jl_register_native(vm, "analog", b_analog, 1);
    jl_register_native(vm, "distance", b_distance, 2);
    jl_register_native(vm, "led", b_led, 3);
    jl_register_native(vm, "led_clear", b_led_clear, 0);
    jl_register_native(vm, "led_show", b_led_show, 0);
    jl_register_native(vm, "led_width", b_led_width, 0);
    jl_register_native(vm, "led_height", b_led_height, 0);
    jl_register_native(vm, "led_size", b_led_size, 0);
    jl_register_native(vm, "led_text", b_led_text, 4);
}
