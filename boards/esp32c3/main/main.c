/*
 * JLang native code on an ESP32-C3 (a RISC-V chip), using ESP-IDF.
 *
 *   jlang build app.jlang -o main/program.s
 *   idf.py set-target esp32c3
 *   idf.py build flash monitor
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jl_hal.h"

void jl_hal_putc(int c) {
    putchar(c);
    if (c == '\n') fflush(stdout);
}

void jl_hal_halt(int code) {
    printf("\n[JLang program stopped with code %d]\n", code);
    fflush(stdout);
    for (;;) vTaskDelay(portMAX_DELAY);
}

void jl_hal_pin_mode(int pin, int mode) {
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, mode == 1 ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, mode == 2 ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
}

void jl_hal_pin_write(int pin, int level) { gpio_set_level((gpio_num_t)pin, (uint32_t)level); }

int jl_hal_pin_read(int pin) { return gpio_get_level((gpio_num_t)pin); }

void jl_hal_delay(int ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks > 0) vTaskDelay(ticks);
    else esp_rom_delay_us((uint32_t)ms * 1000u); /* shorter than one tick */
}

int jl_hal_millis(void) { return (int)(esp_timer_get_time() / 1000); }

void app_main(void) {
    jl_main();
}
