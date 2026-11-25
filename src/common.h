/*
 * common.h - settings shared by every part of JLang.
 *
 * Every limit lives here so you can shrink JLang for a small
 * microcontroller (ESP32) or grow it on a laptop. Each one can be
 * overridden at compile time, e.g.  cc -DJL_STACK_MAX=256 ...
 */
#ifndef JL_COMMON_H
#define JL_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JL_VERSION "0.1.1"

/* How many values the VM stack can hold (each value is 8 bytes). */
#ifndef JL_STACK_MAX
#define JL_STACK_MAX 1024
#endif

/* How many function calls can be active at once (recursion depth). */
#ifndef JL_FRAMES_MAX
#define JL_FRAMES_MAX 64
#endif

/* Maximum number of global names (variables + functions + built-ins). */
#ifndef JL_GLOBALS_MAX
#define JL_GLOBALS_MAX 256
#endif

/* Maximum number of heap blocks that can be alive at the same time. */
#ifndef JL_HANDLES_MAX
#define JL_HANDLES_MAX 1024
#endif

/* Maximum number of `on <event>` handlers. */
#ifndef JL_EVENTS_MAX
#define JL_EVENTS_MAX 16
#endif

/* How deeply expressions and blocks can nest. The compiler calls itself
   for each level, so this bounds how much of the C stack it uses, which
   matters on a microcontroller with a few kilobytes of stack. */
#define JL_STR2(x) #x
#define JL_STR(x) JL_STR2(x)
#ifndef JL_NESTING_MAX
#define JL_NESTING_MAX 100
#endif

/* Struct types per program, and fields per struct. */
#ifndef JL_STRUCTS_MAX
#define JL_STRUCTS_MAX 64
#endif
#ifndef JL_FIELDS_MAX
#define JL_FIELDS_MAX 32
#endif

/* Maximum number of `every` and `after` timers. */
#ifndef JL_TIMERS_MAX
#define JL_TIMERS_MAX 16
#endif

/* Maximum number of built-in (native C) functions. */
#ifndef JL_NATIVES_MAX
#define JL_NATIVES_MAX 128
#endif

/* Default size of the JLang heap in bytes. */
#ifndef JL_DEFAULT_HEAP
#define JL_DEFAULT_HEAP (64u * 1024u)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define JL_PRINTF(fmt_index, first_arg) \
    __attribute__((format(printf, fmt_index, first_arg)))
#else
#define JL_PRINTF(fmt_index, first_arg)
#endif

#endif
