/*
 * panel.h - a simulated LED panel, shared by the VM's board simulator and
 * the RISC-V emulator so `jlang run` and `jlang emu` show the same frames.
 *
 * Programs draw into a frame with led(x, y, colour), then led_show() sends
 * the frame to the panel. Pixels outside the panel are ignored, so text and
 * pictures can scroll in and out of view.
 */
#ifndef JL_PANEL_H
#define JL_PANEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_DEFAULT_W 60
#define PANEL_DEFAULT_H 7
#define PANEL_MAX_W 256
#define PANEL_MAX_H 64

/* Receives each frame as it is shown: ms is the program's time, rgb holds
   w * h colours as 0xRRGGBB, row by row from the top left. */
typedef void (*JlFrameFn)(void *user_data, uint32_t ms, int w, int h, const uint32_t *rgb);

typedef void (*JlPanelWriteFn)(void *user_data, const char *data, size_t len);

typedef struct {
    int w, h;
    uint32_t *rgb;                           /* the frame being drawn, w * h */
    uint32_t frames;                         /* frames shown so far */
    bool print;                              /* print each frame as text */
    bool ansi;                               /* in colour, redrawn in place */
    JlFrameFn sink;                          /* or hand each frame to the host */
    void *sink_data;
} Panel;

bool panel_init(Panel *p, int w, int h); /* false if out of memory */
void panel_free(Panel *p);
void panel_set(Panel *p, int x, int y, uint32_t rgb);
void panel_clear(Panel *p);
void panel_show(Panel *p, uint32_t ms, JlPanelWriteFn write, void *write_data);

#endif
