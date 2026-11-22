#ifndef JL_RV_CODEGEN_H
#define JL_RV_CODEGEN_H

#include "vm.h"

typedef struct {
    bool sim;            /* include _start and the simulated-board HAL */
    const char **emits;  /* events to fire after jl_main (sim only) */
    int nemits;
} RvGenOptions;

/* Compiles a JLang program to RV32IM assembly. Returns a malloc'd
 * string, or NULL after printing errors through the VM. */
char *rv_generate(JlVM *vm, const char *source, const RvGenOptions *opt);

#endif
