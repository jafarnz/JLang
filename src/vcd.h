/*
 * vcd.h - record what the simulated board's pins did, as a Value Change
 * Dump (VCD) file that waveform viewers such as GTKWave open.
 *
 * Signals are created the first time they change, so the header (which
 * must list every signal before the first change) is written at the end.
 */
#ifndef JL_VCD_H
#define JL_VCD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct Vcd Vcd;

Vcd *vcd_new(void);
void vcd_free(Vcd *v);

/* `name` changed to `value` at time `ms`. `width` is its size in bits. */
void vcd_change(Vcd *v, uint32_t ms, const char *name, int width, uint32_t value);

/* Writes the whole dump. Returns false if the file couldn't be written. */
bool vcd_write(const Vcd *v, FILE *f);

/* The same, handed to `out` piece by piece (for the browser build). */
typedef void (*VcdWriteFn)(void *ud, const char *data, size_t len);
void vcd_emit(const Vcd *v, VcdWriteFn out, void *ud);

#endif
