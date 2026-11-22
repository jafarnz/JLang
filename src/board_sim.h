/*
 * board_sim.h - a simulated microcontroller board for the desktop.
 *
 * Gives scripts the same hardware built-ins the real device firmware
 * provides (pins, time, camera, AI, speaker), so a script can be
 * written and tested on a laptop and then run unchanged on the device.
 */
#ifndef JL_BOARD_SIM_H
#define JL_BOARD_SIM_H

#include "jlang.h"

#define SIM_PINS 40

typedef struct {
    bool realtime;               /* delay() really waits */
    int pin_inputs[SIM_PINS];    /* value pin_read() returns, -1 = default */
    const char *ai_answer;       /* what the mocked ask() replies */
    int analog_inputs[SIM_PINS]; /* value analog() returns, -1 = 2048 (a knob halfway) */
    int distance_cm;             /* what distance() measures */
} BoardSimOptions;

void board_sim_default_options(BoardSimOptions *o);
void board_sim_register(JlVM *vm, const BoardSimOptions *o);

#endif
