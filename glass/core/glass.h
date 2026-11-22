/*
 * glass.h - runs a JLang program one instruction at a time and lets a
 * second JLang program, the display, watch it.
 *
 * Glass uses two VMs. The display VM runs glass.jlang, which drives the
 * LEDs and the screen. The program VM runs the program being shown.
 * Before each of the program's instructions, the display VM's `on tick`
 * handler runs. It reads the program's state with the prog_* built-ins
 * below, draws it, and waits as long as the clock knob says. Everything
 * about how Glass looks and how fast it runs is therefore JLang code.
 *
 * Built-ins this adds to the display VM:
 *
 *   prog_line()          source line of the next instruction
 *   prog_depth()         values on the program's stack
 *   prog_value(i)        stack value i (0 = top) as an int
 *   prog_kind(i)         0 int, 1 bool, 2 nil, 3 str, 4 buf, 5 fn, 6 array
 *   prog_frames()        active function calls
 *   prog_cycles()        instructions executed so far
 *   prog_heap()          heap bytes the program has allocated
 *   prog_state()         0 not started, 1 running, 2 finished, 3 error
 *   prog_text(what, b)   copy text into buf b, return its length. what is
 *                        "op" (next instruction), "src" (its source line),
 *                        "out" (last line printed) or "err" (the error)
 *   skip(n)              run the next n instructions without ticks
 *
 * The host adds the hardware built-ins (pins, knob(), i2c_write, ...).
 */
#ifndef GLASS_H
#define GLASS_H

#include "jlang.h"

/* Adds the prog_* built-ins to the display VM. */
void glass_register(JlVM *display);

/*
 * Runs `source` in a new program VM, firing the display's `start`, then
 * `tick` before every instruction, then `done`. Program output goes to
 * `echo` as well as to prog_text("out"). Returns the program's result.
 */
JlResult glass_run(JlVM *display, const char *source, uint32_t heap_size, JlWriteFn echo);

/* How many ticks the display has handled since startup. */
uint64_t glass_ticks(void);

#endif
