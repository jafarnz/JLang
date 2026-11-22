/*
 * The JLang VM on any ESP32, the way Iris uses it: the firmware provides
 * the hardware built-ins, and the behaviour is the JLang script in
 * main/app.jlang, which is built into the firmware.
 *
 *   idf.py set-target esp32s3     (or esp32, esp32c3, esp32c6, ...)
 *   idf.py build flash monitor
 *
 * Pressing the BOOT button fires the script's `on button` handler.
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "esp_random.h"
#include "hardware.h"
#include "jlang.h"

/* The BOOT button: GPIO 0 on Xtensa chips, GPIO 9 on the RISC-V ones. */
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2
#define BUTTON_PIN 9
#else
#define BUTTON_PIN 0
#endif

/* Where the peripheral registers are. mmio_* refuses any other address,
   so a typo becomes a JLang error instead of a crash. */
#if CONFIG_IDF_TARGET_ESP32
#define MMIO_LO 0x3FF00000u
#define MMIO_HI 0x3FFFFFFFu
#else
#define MMIO_LO 0x60000000u
#define MMIO_HI 0x600FFFFFu
#endif

extern const char script_start[] asm("_binary_app_jlang_start");

static void write_out(void *ud, const char *data, size_t len) {
    (void)ud;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

static bool get_pin(JlVM *vm, const char *fn, Value v, int32_t *pin) {
    if (!jl_get_int(vm, fn, 1, v, pin)) return false;
    if (!GPIO_IS_VALID_GPIO(*pin)) return jl_error(vm, "%s(): GPIO %d doesn't exist on this chip", fn, (int)*pin);
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
    bool output = n == 3 && memcmp(mode, "out", 3) == 0;
    bool input = n == 2 && memcmp(mode, "in", 2) == 0;
    bool pullup = n == 9 && memcmp(mode, "in_pullup", 9) == 0;
    if (!output && !input && !pullup)
        return jl_error(vm, "pin_mode(): mode must be \"out\", \"in\" or \"in_pullup\"");
    if (output && !GPIO_IS_VALID_OUTPUT_GPIO(pin))
        return jl_error(vm, "pin_mode(): GPIO %d can only be an input", (int)pin);
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

/* ---- registers -------------------------------------------------------- */

static bool get_addr(JlVM *vm, const char *fn, Value v, uint32_t align, volatile uint32_t **reg, uint32_t *shift) {
    int32_t a;
    if (!jl_get_int(vm, fn, 1, v, &a)) return false;
    uint32_t addr = (uint32_t)a;
    if (addr < MMIO_LO || addr > MMIO_HI)
        return jl_error(vm, "%s(): 0x%08X is outside the peripheral registers (0x%08X to 0x%08X)", fn,
                        (unsigned)addr, (unsigned)MMIO_LO, (unsigned)MMIO_HI);
    if (addr % align != 0)
        return jl_error(vm, "%s(): 0x%08X isn't a multiple of %u", fn, (unsigned)addr, (unsigned)align);
    *reg = (volatile uint32_t *)(uintptr_t)(addr & ~3u);
    *shift = (addr & 3u) * 8;
    return true;
}

/* Peripheral registers are 32 bits wide, so 8-bit access reads or
   modifies one byte of the containing word. */
static bool b_mmio_read32(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    volatile uint32_t *reg;
    uint32_t shift;
    if (!get_addr(vm, "mmio_read32", args[0], 4, &reg, &shift)) return false;
    *out = int_val((int32_t)*reg);
    return true;
}

static bool b_mmio_write32(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    volatile uint32_t *reg;
    uint32_t shift;
    int32_t v;
    if (!get_addr(vm, "mmio_write32", args[0], 4, &reg, &shift) || !jl_get_int(vm, "mmio_write32", 2, args[1], &v))
        return false;
    *reg = (uint32_t)v;
    return true;
}

static bool b_mmio_read8(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    volatile uint32_t *reg;
    uint32_t shift;
    if (!get_addr(vm, "mmio_read8", args[0], 1, &reg, &shift)) return false;
    *out = int_val((int32_t)((*reg >> shift) & 0xFFu));
    return true;
}

static bool b_mmio_write8(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    (void)out;
    volatile uint32_t *reg;
    uint32_t shift;
    int32_t v;
    if (!get_addr(vm, "mmio_write8", args[0], 1, &reg, &shift) || !jl_get_int(vm, "mmio_write8", 2, args[1], &v))
        return false;
    uint32_t word = *reg & ~(0xFFu << shift);
    *reg = word | (((uint32_t)v & 0xFFu) << shift);
    return true;
}

void app_main(void) {
    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = 32 * 1024; /* the script can never use more than this */
    cfg.max_cycles = 0;        /* scripts on a device run forever */
    cfg.out = write_out;
    cfg.err = write_out;
    JlVM *vm = jl_new(&cfg);
    jl_register_native(vm, "pin_mode", b_pin_mode, 2);
    jl_register_native(vm, "pin_write", b_pin_write, 2);
    jl_register_native(vm, "pin_read", b_pin_read, 1);
    jl_register_native(vm, "delay", b_delay, 1);
    jl_register_native(vm, "millis", b_millis, 0);
    jl_register_native(vm, "mmio_read32", b_mmio_read32, 1);
    jl_register_native(vm, "mmio_write32", b_mmio_write32, 2);
    jl_register_native(vm, "mmio_read8", b_mmio_read8, 1);
    jl_register_native(vm, "mmio_write8", b_mmio_write8, 2);
    hardware_register(vm);
    jl_seed_random(vm, esp_random());

    /* Give the USB serial port a moment so the first lines aren't lost. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("\nJLang VM on %s\n", CONFIG_IDF_TARGET);

    if (jl_run_source(vm, script_start) != JL_OK)
        printf("The script stopped with an error. Fix app.jlang and flash again.\n");

    /* Poll the BOOT button and fire the script's event on each press. */
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
