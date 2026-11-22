/*
 * Glass firmware for a Seeed Studio XIAO ESP32-C6.
 *
 * Runs glass.jlang, the display program, then runs program.jlang over and
 * over with the display watching each instruction (see core/glass.h).
 * This file only provides the hardware built-ins the JLang code uses:
 *
 *   pin_mode, pin_write, pin_read   GPIO
 *   delay(ms), millis()             time
 *   knob()                          the potentiometer on D0, 0 to 4095
 *   i2c_write(addr, control, bytes, offset, count)
 *                                   send `control` and then `count` bytes
 *                                   of a buf or str over I2C (SDA D4, SCL D5)
 *
 *   idf.py build flash monitor
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "glass.h"
#include "jlang.h"

#define KNOB_GPIO 0  /* D0 */
#define I2C_SDA 22   /* D4 */
#define I2C_SCL 23   /* D5 */
#define I2C_HZ 400000

extern const char glass_src[] asm("_binary_glass_jlang_start");
extern const char program_src[] asm("_binary_program_jlang_start");

static void write_out(void *ud, const char *data, size_t len) {
    (void)ud;
    fwrite(data, 1, len, stdout);
    fflush(stdout);
}

/* ---- pins and time --------------------------------------------------------- */

static bool get_pin(JlVM *vm, const char *fn, Value v, int32_t *pin) {
    if (!jl_get_int(vm, fn, 1, v, pin)) return false;
    if (!GPIO_IS_VALID_GPIO(*pin)) return jl_error(vm, "%s(): GPIO %d doesn't exist on this chip", fn, (int)*pin);
    return true;
}

static bool b_pin_mode(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
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
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)pin, pullup ? GPIO_PULLUP_ONLY : GPIO_FLOATING);
    return true;
}

static bool b_pin_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
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
    (void)argc, (void)out;
    int32_t ms;
    if (!jl_get_int(vm, "delay", 1, args[0], &ms)) return false;
    if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1);
    return true;
}

static bool b_millis(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val((int32_t)(esp_timer_get_time() / 1000));
    return true;
}

/* ---- the knob -------------------------------------------------------------- */

static adc_oneshot_unit_handle_t adc;
static adc_channel_t knob_channel;

static void knob_init(void) {
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(KNOB_GPIO, &unit, &knob_channel));
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &adc));
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, knob_channel, &ccfg));
}

static bool b_knob(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)args;
    int raw;
    if (adc_oneshot_read(adc, knob_channel, &raw) != ESP_OK) return jl_error(vm, "knob(): reading the ADC failed");
    *out = int_val(raw);
    return true;
}

/* ---- I2C ------------------------------------------------------------------- */

static i2c_master_bus_handle_t bus;
static struct {
    int addr;
    i2c_master_dev_handle_t dev;
} devices[4];
static uint8_t i2c_buf[1 + 1024];

static void i2c_init(void) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
}

static i2c_master_dev_handle_t device(int addr) {
    for (int i = 0; i < 4; i++)
        if (devices[i].dev && devices[i].addr == addr) return devices[i].dev;
    for (int i = 0; i < 4; i++) {
        if (devices[i].dev) continue;
        i2c_device_config_t dcfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = (uint16_t)addr,
            .scl_speed_hz = I2C_HZ,
        };
        if (i2c_master_bus_add_device(bus, &dcfg, &devices[i].dev) != ESP_OK) return NULL;
        devices[i].addr = addr;
        return devices[i].dev;
    }
    return NULL;
}

static bool b_i2c_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t addr, control, off, n;
    uint8_t *data;
    uint32_t len;
    if (!jl_get_int(vm, "i2c_write", 1, args[0], &addr) || !jl_get_int(vm, "i2c_write", 2, args[1], &control) ||
        !jl_get_bytes(vm, "i2c_write", args[2], &data, &len) || !jl_get_int(vm, "i2c_write", 4, args[3], &off) ||
        !jl_get_int(vm, "i2c_write", 5, args[4], &n))
        return false;
    if (addr < 0 || addr > 0x7F) return jl_error(vm, "i2c_write(): 0x%X isn't a 7-bit I2C address", (unsigned)addr);
    if (off < 0 || n < 0 || (uint32_t)off + (uint32_t)n > len)
        return jl_error(vm, "i2c_write(): bytes %d..%d are outside the %u-byte buffer", (int)off, (int)(off + n - 1),
                        (unsigned)len);
    if (n > (int32_t)sizeof i2c_buf - 1) return jl_error(vm, "i2c_write(): at most 1024 bytes at a time");
    i2c_master_dev_handle_t dev = device(addr);
    if (dev == NULL) return jl_error(vm, "i2c_write(): too many different I2C addresses");
    i2c_buf[0] = (uint8_t)control;
    memcpy(i2c_buf + 1, data + off, (size_t)n);
    if (i2c_master_transmit(dev, i2c_buf, (size_t)n + 1, 50) != ESP_OK)
        return jl_error(vm, "i2c_write(): no device answered at address 0x%02X. Check the wiring of SDA (D4), "
                            "SCL (D5), VCC and GND", (unsigned)addr);
    return true;
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the USB serial port connect */
    printf("\nGlass\n");
    knob_init();
    i2c_init();

    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = 16 * 1024;
    cfg.max_cycles = 0;
    cfg.out = write_out;
    cfg.err = write_out;
    JlVM *display = jl_new(&cfg);
    jl_register_native(display, "pin_mode", b_pin_mode, 2);
    jl_register_native(display, "pin_write", b_pin_write, 2);
    jl_register_native(display, "pin_read", b_pin_read, 1);
    jl_register_native(display, "delay", b_delay, 1);
    jl_register_native(display, "millis", b_millis, 0);
    jl_register_native(display, "knob", b_knob, 0);
    jl_register_native(display, "i2c_write", b_i2c_write, 5);
    glass_register(display);

    gpio_reset_pin(1); /* the step button, D1 */
    gpio_set_direction(1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(1, GPIO_PULLUP_ONLY);

    if (jl_run_source(display, glass_src) != JL_OK) {
        printf("glass.jlang stopped with an error, so Glass can't start.\n");
        return;
    }
    for (;;) {
        glass_run(display, program_src, 16 * 1024, write_out);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
