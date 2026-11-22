/*
 * JLang native code on a RISC-V ESP32 (ESP32-C3, C6 or H2), using ESP-IDF.
 * There is no interpreter on the chip: main/app.jlang is compiled to RISC-V
 * machine code by `jlang build` during the firmware build.
 *
 *   idf.py set-target esp32c6     (or esp32c3, esp32h2)
 *   idf.py build flash monitor
 *
 * Pressing the BOOT button calls the program's `on button` handler, if it
 * has one.
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "jl_hal.h"
#include "led_strip.h"
#include <string.h>

#define BUTTON_PIN 9 /* BOOT on every RISC-V ESP32 */

/* Generated only when the program has an `on button` handler. */
extern void jl_on_button(void) __attribute__((weak));

void jl_hal_putc(int c) {
    putchar(c);
    if (c == '\n') fflush(stdout);
}

void jl_hal_halt(int code) {
    printf("\nThe program stopped with code %d. Fix app.jlang and flash again.\n", code);
    fflush(stdout);
    for (;;) vTaskDelay(portMAX_DELAY);
}

void jl_hal_pin_mode(int pin, int mode) {
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, mode == 1 ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, mode == 2 ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}

void jl_hal_pin_write(int pin, int level) { gpio_set_level((gpio_num_t)pin, (uint32_t)(level != 0)); }

int jl_hal_pin_read(int pin) { return gpio_get_level((gpio_num_t)pin); }

void jl_hal_delay(int ms) {
    if (ms <= 0) return;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks > 0) vTaskDelay(ticks);
    else esp_rom_delay_us((uint32_t)ms * 1000u); /* shorter than one tick */
}

int jl_hal_millis(void) { return (int)(esp_timer_get_time() / 1000); }

/* The LED panel: a WS2812 matrix, by default the common 8x32 kind wired as a
   zigzag down one column and up the next. Only linked in if a program uses it. */
#define PANEL_PIN 2
#define PANEL_W 32
#define PANEL_H 8
#define PANEL_ZIGZAG 1
#define PANEL_BRIGHTNESS 24 /* of 255: a full panel at full white needs more than USB gives */

static uint32_t panel[PANEL_W * PANEL_H];
static led_strip_handle_t panel_strip;

void jl_hal_led_set(int x, int y, int rgb) {
    if (x >= 0 && y >= 0 && x < PANEL_W && y < PANEL_H) panel[y * PANEL_W + x] = (uint32_t)rgb;
}

void jl_hal_led_clear(void) { memset(panel, 0, sizeof panel); }

void jl_hal_led_show(void) {
    if (panel_strip == NULL) {
        led_strip_config_t cfg = {
            .strip_gpio_num = PANEL_PIN,
            .max_leds = PANEL_W * PANEL_H,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rmt = {.clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000};
        if (led_strip_new_rmt_device(&cfg, &rmt, &panel_strip) != ESP_OK) return;
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
}

int jl_hal_led_width(void) { return PANEL_W; }
int jl_hal_led_height(void) { return PANEL_H; }

void app_main(void) {
    /* Give the USB serial port a moment so the first lines aren't lost. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("\nJLang native code on %s\n", CONFIG_IDF_TARGET);
    jl_main();
    if (!jl_on_button) return;

    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    int last = 1;
    for (;;) {
        int now = gpio_get_level(BUTTON_PIN);
        if (last == 1 && now == 0) jl_on_button();
        last = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
