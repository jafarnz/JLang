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
#include "panel.h"
#include "vcd.h"

#define SIM_PINS 40

typedef struct {
    bool realtime;               /* delay() really waits */
    int pin_inputs[SIM_PINS];    /* value pin_read() returns, -1 = default */
    const char *ai_answer;       /* what the mocked ask() replies */
    int analog_inputs[SIM_PINS]; /* value analog() returns, -1 = 2048 (a knob halfway) */
    int distance_cm;             /* what distance() measures */
    int panel_w, panel_h;        /* LED panel size, default 60 x 7 */
    bool print_frames;           /* print each led_show() frame as text */
    bool ansi_frames;            /* ... in colour, redrawn in place */
    JlFrameFn frame_sink;        /* also hand each frame to the host */
    void *frame_data;
    uint32_t max_ms;             /* stop once this much time has passed, 0 = never */
    Vcd *vcd;                    /* record pin changes for a waveform viewer, or NULL */
    int temperature_c;           /* what the simulated I2C temperature sensor measures */
} BoardSimOptions;

void board_sim_default_options(BoardSimOptions *o);
void board_sim_register(JlVM *vm, const BoardSimOptions *o);

/* Runs the script's every/after timers in simulated time until none are
   left or the clock reaches `until_ms` (0 = no limit). */
JlResult board_sim_run_timers(JlVM *vm, uint32_t until_ms);

#endif
