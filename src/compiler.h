#ifndef JL_COMPILER_H
#define JL_COMPILER_H

#include "vm.h"

/* Compiles source into vm->prog. Returns the index of the top-level
 * script function, or -1 if there were compile errors. */
int jl_compile(JlVM *vm, const char *source);

#endif
