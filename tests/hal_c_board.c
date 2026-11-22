/* A board HAL written in C, like a real one, but for the emulator's registers. */
#define REG(a) (*(volatile unsigned *)(a))
void jl_main(void);
void jl_on_press(void);
void jl_hal_putc(int c) { REG(0x10000000u) = (unsigned)c; }
void jl_hal_halt(int code) { for (;;) REG(0x10003000u) = (unsigned)code; }
void jl_hal_pin_mode(int pin, int mode) {
    unsigned bit = 1u << pin;
    unsigned d = REG(0x10001000u) & ~bit, p = REG(0x10001004u) & ~bit;
    if (mode == 1) d |= bit;
    if (mode == 2) p |= bit;
    REG(0x10001000u) = d;
    REG(0x10001004u) = p;
}
void jl_hal_pin_write(int pin, int level) {
    unsigned bit = 1u << pin, o = REG(0x10001008u) & ~bit;
    REG(0x10001008u) = level ? o | bit : o;
}
int jl_hal_pin_read(int pin) { return (int)((REG(0x1000100Cu) >> pin) & 1u); }
void jl_hal_delay(int ms) { REG(0x10002004u) = (unsigned)ms; }
int jl_hal_millis(void) { return (int)REG(0x10002000u); }
/* Like a board's main(): run the program, then fire an event twice,
 * checking that callee-saved registers survive the calls. */
void _start(void) {
    register int keep asm("s1") = 1234;
    asm volatile("" : "+r"(keep));
    jl_main();
    jl_on_press();
    jl_on_press();
    asm volatile("" : "+r"(keep));
    jl_hal_halt(keep == 1234 ? 0 : 99);
}
