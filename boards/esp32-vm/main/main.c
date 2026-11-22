/*
 * The JLang VM on any ESP32, the way Iris uses it: the firmware provides
 * hardware built-ins, and the behaviour is a JLang script.
 *
 *   idf.py set-target esp32s3     (or esp32, esp32c3, ...)
 *   idf.py build flash monitor
 *
 * A button between BUTTON_PIN and ground fires the script's `on button`.
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jlang.h"

#define BUTTON_PIN 0 /* the BOOT button on most dev boards */

extern const char script_start[] asm("_binary_main_jlang_start");

static void write_out(void *ud, const char *data, size_t len) {
    (void)ud;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

static bool get_pin(JlVM *vm, const char *fn, Value v, int32_t *pin) {
    if (!jl_get_int(vm, fn, 1, v, pin)) return false;
    if (!GPIO_IS_VALID_GPIO(*pin)) return jl_error(vm, "%s(): GPIO %d doesn't exist on this chip", fn, *pin);
    return true;
}

static bool b_pin_mode(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t pin;
    const char *mode;
    uint32_t n;
    if (!get_pin(vm, "pin_mode", args[0], &pin)) return false;
    if (!jl_get_string(vm, args[1], &mode, &n)) return jl_error(vm, "pin_mode(): mode must be a string");
    bool output = n == 3;
    bool pullup = n == 9;
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
    return true;
}

static bool b_pin_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t pin, level;
    if (!get_pin(vm, "pin_write", args[0], &pin) || !jl_get_int(vm, "pin_write", 2, args[1], &level)) return false;
    gpio_set_level((gpio_num_t)pin, level != 0);
    return true;
}

static bool b_pin_read(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "pin_read", args[0], &pin)) return false;
    *out = int_val(gpio_get_level((gpio_num_t)pin));
    return true;
}

static bool b_delay(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    int32_t ms;
    if (!jl_get_int(vm, "delay", 1, args[0], &ms)) return false;
    vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 0));
    return true;
}

static bool b_millis(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm;
    (void)argc;
    (void)args;
    *out = int_val((int32_t)(esp_timer_get_time() / 1000));
    return true;
}

void app_main(void) {
    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = 32 * 1024;     /* the script can never use more than this */
    cfg.max_cycles = 0;
    cfg.out = write_out;
    cfg.err = write_out;
    JlVM *vm = jl_new(&cfg);
    jl_register_native(vm, "pin_mode", b_pin_mode, 2);
    jl_register_native(vm, "pin_write", b_pin_write, 2);
    jl_register_native(vm, "pin_read", b_pin_read, 1);
    jl_register_native(vm, "delay", b_delay, 1);
    jl_register_native(vm, "millis", b_millis, 0);

    if (jl_run_source(vm, script_start) != JL_OK) {
        printf("The script stopped with an error. The firmware is still running.\n");
    }

    /* Poll the button and fire the script's event when it is pressed. */
    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    int last = 1;
    for (;;) {
        int now = gpio_get_level(BUTTON_PIN);
        if (last == 1 && now == 0) jl_emit(vm, "button");
        last = now;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
