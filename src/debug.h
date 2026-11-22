#ifndef JL_DEBUG_H
#define JL_DEBUG_H

#include "vm.h"

const char *opcode_name(uint8_t op);
void disassemble_function(JlVM *vm, int fn);
int disassemble_instruction(JlVM *vm, int fn, int offset);

#endif
