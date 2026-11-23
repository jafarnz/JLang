/*
 * The JLang VM on any ESP32, the way Iris uses it: the firmware provides
 * the hardware built-ins, and the behaviour is the JLang script in
 * main/app.jlang, which is built into the firmware.
 *
 *   idf.py set-target esp32s3     (or esp32, esp32c3, esp32c6, ...)
 *   idf.py build flash monitor
 *
 * Pressing the BOOT button fires the script's `on button` handler.
 *
 * The playground at jafarniaz.com/jlang/playground can also send a
 * program over USB (Web Serial), which replaces the built-in one. The
 * protocol is plain text, so it can be typed in a serial monitor too:
 *
 *   computer -> board   @jlang run <bytes>\n<source>   run a program
 *                       @jlang save <bytes>\n<source>  run it, and keep it after a reset
 *                       @jlang forget\n                go back to the built-in program
 *                       @jlang stop\n                  stop the running program
 *   board -> computer   @jlang ready <chip>             after boot
 *                       @jlang started / @jlang finished <code>
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
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

/* The program currently loaded, and one waiting to replace it. */
#define PROGRAM_MAX (32 * 1024)
static JlVM *vm;
static char *volatile pending;   /* a program the reader task received */
static volatile bool pending_save;
static volatile bool stop_now;   /* "@jlang stop" */

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
    /* Wait in short steps, so a new program doesn't wait for a long delay. */
    int64_t end = esp_timer_get_time() + (int64_t)(ms > 0 ? ms : 0) * 1000;
    while (esp_timer_get_time() < end) {
        if (pending || stop_now) return jl_error(vm, "stopped from the computer");
        int64_t left = (end - esp_timer_get_time()) / 1000;
        vTaskDelay(pdMS_TO_TICKS(left > 10 ? 10 : left > 0 ? left : 1));
    }
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

/* ---- programs from the computer ----------------------------------------- */

static int read_byte(void) {
    uint8_t c;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    while (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
    }
#else
    while (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY) != 1) {
    }
#endif
    return c;
}

/* Reads "@jlang ..." commands from USB. Anything else is ignored. */
static void reader_task(void *arg) {
    (void)arg;
    char line[64];
    int n = 0;
    for (;;) {
        int c = read_byte();
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < (int)sizeof line - 1) line[n++] = (char)c;
            continue;
        }
        line[n] = '\0';
        n = 0;
        unsigned len = 0;
        bool save = sscanf(line, "@jlang save %u", &len) == 1;
        if (save || sscanf(line, "@jlang run %u", &len) == 1) {
            if (len == 0 || len > PROGRAM_MAX) {
                printf("@jlang error a program can be at most %d bytes\n", PROGRAM_MAX);
                continue;
            }
            char *src = malloc(len + 1);
            for (unsigned i = 0; i < len; i++) {
                int b = read_byte();
                if (src) src[i] = (char)b;
            }
            if (src == NULL) {
                printf("@jlang error out of memory\n");
                continue;
            }
            src[len] = '\0';
            while (pending) vTaskDelay(pdMS_TO_TICKS(5)); /* the last one is still loading */
            pending_save = save;
            pending = src;
            if (vm) jl_request_stop(vm);
        } else if (strcmp(line, "@jlang stop") == 0) {
            stop_now = true;
            if (vm) jl_request_stop(vm);
        } else if (strcmp(line, "@jlang forget") == 0) {
            nvs_handle_t h;
            if (nvs_open("jlang", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, "program");
                nvs_commit(h);
                nvs_close(h);
            }
            printf("@jlang forgot the saved program; reset the board to run the built-in one\n");
        }
    }
}

static void start_reader(void) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 4096;
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
#else
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 4096, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
    xTaskCreate(reader_task, "jlang_usb", 4096, NULL, 5, NULL);
}

/* A program saved with "@jlang save", or NULL. */
static char *saved_program(void) {
    nvs_handle_t h;
    if (nvs_open("jlang", NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t len = 0;
    char *src = NULL;
    if (nvs_get_blob(h, "program", NULL, &len) == ESP_OK && len > 0 && (src = malloc(len + 1)) != NULL) {
        if (nvs_get_blob(h, "program", src, &len) == ESP_OK) src[len] = '\0';
        else src = (free(src), NULL);
    }
    nvs_close(h);
    return src;
}

static void save_program(const char *src) {
    nvs_handle_t h;
    if (nvs_open("jlang", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "program", src, strlen(src));
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t clock_ms(void *ud) {
    (void)ud;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* A fresh VM with every built-in, for each program. */
static JlVM *new_vm(void) {
    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = 32 * 1024; /* the script can never use more than this */
    cfg.max_cycles = 0;        /* scripts on a device run forever */
    cfg.out = write_out;
    cfg.err = write_out;
    JlVM *v = jl_new(&cfg);
    if (v == NULL) return NULL;
    jl_register_native(v, "pin_mode", b_pin_mode, 2);
    jl_register_native(v, "pin_write", b_pin_write, 2);
    jl_register_native(v, "pin_read", b_pin_read, 1);
    jl_register_native(v, "delay", b_delay, 1);
    jl_register_native(v, "millis", b_millis, 0);
    jl_register_native(v, "mmio_read32", b_mmio_read32, 1);
    jl_register_native(v, "mmio_write32", b_mmio_write32, 2);
    jl_register_native(v, "mmio_read8", b_mmio_read8, 1);
    jl_register_native(v, "mmio_write8", b_mmio_write8, 2);
    hardware_register(v);
    jl_seed_random(v, esp_random());
    jl_set_clock(v, clock_ms, NULL);
    return v;
}

/* Runs a program's top level. Its timers and events run from app_main. */
static void run_program(const char *src) {
    if (vm) jl_free(vm);
    hardware_reset();
    stop_now = false;
    vm = new_vm();
    if (vm == NULL) {
        printf("@jlang error out of memory\n");
        return;
    }
    printf("@jlang started\n");
    JlResult r = jl_run_source(vm, src);
    printf("@jlang finished %d\n", r == JL_OK ? 0 : r == JL_COMPILE_ERROR ? 65 : 70);
    if (r != JL_OK) {
        /* Nothing more to run: drop its timers and handlers. */
        jl_free(vm);
        vm = NULL;
    }
}

void app_main(void) {
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    start_reader();

    /* Give the USB serial port a moment so the first lines aren't lost. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    printf("\nJLang VM on %s\n@jlang ready %s\n", CONFIG_IDF_TARGET, CONFIG_IDF_TARGET);

    char *saved = saved_program();
    run_program(saved ? saved : script_start);
    free(saved);

    gpio_reset_pin(BUTTON_PIN);
    gpio_set_direction(BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BUTTON_PIN, GPIO_PULLUP_ONLY);
    int last = 1;
    uint32_t last_poll = 0;
    for (;;) {
        if (pending) {
            char *src = pending;
            if (pending_save) save_program(src);
            run_program(src);
            pending = NULL;
            free(src);
        }
        if (stop_now) {
            stop_now = false;
            if (vm) {
                jl_free(vm);
                vm = NULL;
                hardware_reset();
                printf("@jlang finished 0\n");
            }
        }
        /* The BOOT button fires the script's `on button` handler. It is
           read every 20 ms, which also ignores contact bounce. */
        if (clock_ms(NULL) - last_poll >= 20) {
            last_poll = clock_ms(NULL);
            int now = gpio_get_level(BUTTON_PIN);
            if (vm && last == 1 && now == 0 && jl_emit(vm, "button") == JL_RUNTIME_ERROR) {
                jl_free(vm);
                vm = NULL;
            }
            last = now;
        }
        /* every and after blocks. */
        uint32_t due;
        if (vm && jl_next_timer(vm, &due) && (int32_t)(clock_ms(NULL) - due) >= 0 &&
            jl_run_timers(vm, clock_ms(NULL)) != JL_OK) {
            jl_free(vm);
            vm = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(vm && jl_next_timer(vm, &due) ? 1 : 20));
    }
}
