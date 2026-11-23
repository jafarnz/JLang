#ifndef HARDWARE_H
#define HARDWARE_H

#include "jlang.h"

/* Adds the beginner hardware built-ins (see hardware.c). */
void hardware_register(JlVM *vm);

/* Turns off PWM, colour LEDs and the panel, before a new program runs. */
void hardware_reset(void);

#endif
