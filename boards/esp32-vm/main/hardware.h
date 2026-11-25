#ifndef HARDWARE_H
#define HARDWARE_H

#include "jlang.h"

/* Adds the beginner hardware built-ins (see hardware.c). */
void hardware_register(JlVM *vm);

/* Turns off PWM, colour LEDs and the panel, before a new program runs. */
void hardware_reset(void);

/* Implemented in main.c: emits "@jlang pin <n> <0|1>" when live watch is on. */
void board_notify_pin(int pin, int level);

/* Implemented in net_wifi.c: registers wifi_connect, wifi_ip, http_get. */
void net_wifi_register(JlVM *vm);

#endif
