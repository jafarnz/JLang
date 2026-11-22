/*
 * hw.h - helpers for the beginner hardware built-ins, shared by the
 * desktop simulator and device firmware so both accept the same values.
 */
#ifndef JL_HW_H
#define JL_HW_H

#include "jlang.h"

/* A colour: 0xRRGGBB, or a name such as "red", "orange" or "off". */
bool jl_hw_color(JlVM *vm, const char *fn, int arg_no, Value v, uint32_t *rgb);

/* The name of a colour if it has one, otherwise NULL. */
const char *jl_hw_color_name(uint32_t rgb);

/* A frequency in Hz: a number, or a note such as "C4", "F#5" or "Bb3".
   "rest" (or 0) means silence. Allowed range: 0 or 20..20000 Hz. */
bool jl_hw_note(JlVM *vm, const char *fn, int arg_no, Value v, int32_t *hz);

#endif
