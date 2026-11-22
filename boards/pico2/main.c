/* JLang native code on a Raspberry Pi Pico 2 (RP2350, RISC-V mode). */
#include <stdio.h>

#include "jl_hal.h"
#include "pico/stdlib.h"

void jl_hal_putc(int c) { putchar_raw(c); }

void jl_hal_halt(int code) {
    printf("\n[JLang program stopped with code %d]\n", code);
    for (;;) tight_loop_contents();
}

void jl_hal_pin_mode(int pin, int mode) {
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, mode == 1);
    if (mode == 2) gpio_pull_up((uint)pin);
    else gpio_disable_pulls((uint)pin);
}

void jl_hal_pin_write(int pin, int level) { gpio_put((uint)pin, level != 0); }

int jl_hal_pin_read(int pin) { return gpio_get((uint)pin); }

void jl_hal_delay(int ms) { sleep_ms((uint32_t)ms); }

int jl_hal_millis(void) { return (int)to_ms_since_boot(get_absolute_time()); }

int main(void) {
    stdio_init_all();
    sleep_ms(2000); /* give the USB serial port time to connect */
    jl_main();
    for (;;) tight_loop_contents();
}
