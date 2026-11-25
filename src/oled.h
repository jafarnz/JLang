/*
 * oled.h - 128x64 monochrome OLED framebuffer and drawing primitives,
 * shared by the desktop/WASM board simulator and the ESP32 VM firmware.
 *
 * Framebuffer uses the native SH1106/SSD1306 8-page x 128-column layout
 * (1,024 bytes total: page = y / 8, bit = y % 8), so ESP32 firmware can
 * transmit each 128-byte page straight over I2C with zero conversion.
 */
#ifndef JL_OLED_H
#define JL_OLED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jlang.h"

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES 8
#define OLED_FB_SIZE (OLED_W * OLED_PAGES)

typedef struct Oled Oled;
typedef void (*JlOledFrameFn)(void *user_data, uint32_t ms, const uint8_t *fb, bool inverted, uint8_t contrast);
typedef void (*JlOledWriteFn)(void *user_data, const char *data, size_t len);
typedef bool (*JlOledShowHook)(JlVM *vm, Oled *o, void *user_data);

struct Oled {
    uint8_t fb[OLED_FB_SIZE];
    bool inverted;
    uint8_t contrast;
    uint32_t frames;
    bool print;
    JlOledFrameFn sink;
    void *sink_data;
};

void oled_init(Oled *o);
void oled_clear(Oled *o);
void oled_pixel(Oled *o, int x, int y, bool on);
bool oled_get_pixel(const Oled *o, int x, int y);
void oled_line(Oled *o, int x0, int y0, int x1, int y1, bool on);
void oled_rect(Oled *o, int x, int y, int w, int h, bool fill, bool on);
void oled_circle(Oled *o, int cx, int cy, int r, bool fill, bool on);
int oled_text(Oled *o, const char *s, uint32_t n, int x, int y, int scale, bool on);
void oled_sprite_bits(Oled *o, int x, int y, int w, int h, const uint8_t *bytes, uint32_t nbytes, bool on);
void oled_invert(Oled *o, bool inverted);
void oled_contrast(Oled *o, uint8_t contrast);
void oled_show(Oled *o, uint32_t ms, JlOledWriteFn write, void *write_data);

/* Registers oled_* built-ins on vm backed by *o; on_show flushes the frame. */
void oled_register_natives(JlVM *vm, Oled *o, JlOledShowHook on_show, void *hook_ud);

#endif
