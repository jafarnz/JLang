/*
 * oled.c - 128x64 monochrome OLED drawing primitives (see oled.h).
 */
#include "oled.h"

#include "hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oled_init(Oled *o) {
    memset(o, 0, sizeof *o);
    o->contrast = 0xCF;
}

void oled_clear(Oled *o) {
    memset(o->fb, 0, sizeof o->fb);
}

void oled_pixel(Oled *o, int x, int y, bool on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    int idx = (y >> 3) * OLED_W + x;
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) o->fb[idx] |= mask;
    else o->fb[idx] &= (uint8_t)~mask;
}

bool oled_get_pixel(const Oled *o, int x, int y) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return false;
    int idx = (y >> 3) * OLED_W + x;
    return (o->fb[idx] & (uint8_t)(1u << (y & 7))) != 0;
}

static void oled_hline(Oled *o, int x0, int x1, int y, bool on) {
    if (y < 0 || y >= OLED_H) return;
    if (x0 > x1) {
        int tmp = x0;
        x0 = x1;
        x1 = tmp;
    }
    if (x1 < 0 || x0 >= OLED_W) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= OLED_W) x1 = OLED_W - 1;
    int base = (y >> 3) * OLED_W;
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) {
        for (int x = x0; x <= x1; x++) o->fb[base + x] |= mask;
    } else {
        uint8_t keep = (uint8_t)~mask;
        for (int x = x0; x <= x1; x++) o->fb[base + x] &= keep;
    }
}

void oled_line(Oled *o, int x0, int y0, int x1, int y1, bool on) {
    if (y0 == y1) {
        oled_hline(o, x0, x1, y0, on);
        return;
    }
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        oled_pixel(o, x0, y0, on);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            if (x0 == x1) break;
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            if (y0 == y1) break;
            err += dx;
            y0 += sy;
        }
    }
}

void oled_rect(Oled *o, int x, int y, int w, int h, bool fill, bool on) {
    if (w <= 0 || h <= 0) return;
    if (fill) {
        for (int yy = y; yy < y + h; yy++) oled_hline(o, x, x + w - 1, yy, on);
        return;
    }
    oled_hline(o, x, x + w - 1, y, on);
    if (h > 1) oled_hline(o, x, x + w - 1, y + h - 1, on);
    for (int yy = y + 1; yy < y + h - 1; yy++) {
        oled_pixel(o, x, yy, on);
        if (w > 1) oled_pixel(o, x + w - 1, yy, on);
    }
}

void oled_circle(Oled *o, int cx, int cy, int r, bool fill, bool on) {
    if (r < 0) return;
    if (r == 0) {
        oled_pixel(o, cx, cy, on);
        return;
    }
    int x = r;
    int y = 0;
    int err = 1 - x;
    while (x >= y) {
        if (fill) {
            oled_hline(o, cx - x, cx + x, cy + y, on);
            oled_hline(o, cx - x, cx + x, cy - y, on);
            oled_hline(o, cx - y, cx + y, cy + x, on);
            oled_hline(o, cx - y, cx + y, cy - x, on);
        } else {
            oled_pixel(o, cx + x, cy + y, on);
            oled_pixel(o, cx + y, cy + x, on);
            oled_pixel(o, cx - y, cy + x, on);
            oled_pixel(o, cx - x, cy + y, on);
            oled_pixel(o, cx - x, cy - y, on);
            oled_pixel(o, cx - y, cy - x, on);
            oled_pixel(o, cx + y, cy - x, on);
            oled_pixel(o, cx + x, cy - y, on);
        }
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

typedef struct {
    Oled *oled;
    int origin_x;
    int origin_y;
    int scale;
    bool on;
} OledTextCtx;

static void oled_text_plot(void *ud, int px, int py) {
    OledTextCtx *ctx = ud;
    int sx = ctx->origin_x + (px - ctx->origin_x) * ctx->scale;
    int sy = ctx->origin_y + (py - ctx->origin_y) * ctx->scale;
    if (ctx->scale == 1) {
        oled_pixel(ctx->oled, sx, sy, ctx->on);
        return;
    }
    oled_rect(ctx->oled, sx, sy, ctx->scale, ctx->scale, true, ctx->on);
}

int oled_text(Oled *o, const char *s, uint32_t n, int x, int y, int scale, bool on) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    OledTextCtx ctx;
    ctx.oled = o;
    ctx.origin_x = x;
    ctx.origin_y = y;
    ctx.scale = scale;
    ctx.on = on;
    int end_unscaled = jl_hw_text(s, n, x, y, oled_text_plot, &ctx);
    return x + (end_unscaled - x) * scale;
}

void oled_sprite_bits(Oled *o, int x, int y, int w, int h, const uint8_t *bytes, uint32_t nbytes, bool on) {
    if (w <= 0 || h <= 0 || bytes == NULL) return;
    int stride = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t bi = (uint32_t)(row * stride + (col >> 3));
            if (bi >= nbytes) return;
            if ((bytes[bi] & (uint8_t)(0x80u >> (col & 7))) != 0) {
                oled_pixel(o, x + col, y + row, on);
            }
        }
    }
}

void oled_invert(Oled *o, bool inverted) {
    o->inverted = inverted;
}

void oled_contrast(Oled *o, uint8_t contrast) {
    o->contrast = contrast;
}

static uint32_t count_lit(const uint8_t *fb) {
    uint32_t total = 0;
    for (int i = 0; i < OLED_FB_SIZE; i++) {
        uint8_t b = fb[i];
        while (b) {
            b &= (uint8_t)(b - 1);
            total++;
        }
    }
    return total;
}

void oled_show(Oled *o, uint32_t ms, JlOledWriteFn write, void *write_data) {
    o->frames++;
    if (o->sink != NULL) o->sink(o->sink_data, ms, o->fb, o->inverted, o->contrast);
    if (!o->print || write == NULL) return;
    uint32_t lit = count_lit(o->fb);
    char line[96];
    int n = snprintf(line, sizeof line, "[sim %6ums] oled frame %u (%u px lit%s)\n",
                     (unsigned)ms, (unsigned)o->frames, (unsigned)lit,
                     o->inverted ? ", inverted" : "");
    write(write_data, line, (size_t)n);
}

/* ---- Shared JLang VM built-in bindings ---------------------------------- */

static Oled *active_oled;
static JlOledShowHook active_show_hook;
static void *active_show_ud;

static bool get_flag(JlVM *vm, const char *fn, int arg_no, Value v, bool *out) {
    if (v.type == VAL_BOOL) {
        *out = v.as.b != 0;
        return true;
    }
    if (v.type == VAL_INT) {
        *out = v.as.i != 0;
        return true;
    }
    return jl_error(vm, "%s(): argument %d must be a bool or int, got %s", fn, arg_no, value_type_name(v));
}

static bool nat_oled_clear(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args; (void)out;
    if (active_oled) oled_clear(active_oled);
    return true;
}

static bool nat_oled_width(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    *out = int_val(OLED_W);
    return true;
}

static bool nat_oled_height(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm; (void)argc; (void)args;
    *out = int_val(OLED_H);
    return true;
}

static bool nat_oled_pixel(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    if (argc < 2 || argc > 3) return jl_error(vm, "oled_pixel(): expects 2 or 3 arguments (x, y[, on])");
    int32_t x, y;
    bool on = true;
    if (!jl_get_int(vm, "oled_pixel", 1, args[0], &x) ||
        !jl_get_int(vm, "oled_pixel", 2, args[1], &y)) return false;
    if (argc == 3 && !get_flag(vm, "oled_pixel", 3, args[2], &on)) return false;
    if (active_oled) oled_pixel(active_oled, (int)x, (int)y, on);
    return true;
}

static bool nat_oled_line(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    if (argc < 4 || argc > 5) return jl_error(vm, "oled_line(): expects 4 or 5 arguments (x0, y0, x1, y1[, on])");
    int32_t x0, y0, x1, y1;
    bool on = true;
    if (!jl_get_int(vm, "oled_line", 1, args[0], &x0) ||
        !jl_get_int(vm, "oled_line", 2, args[1], &y0) ||
        !jl_get_int(vm, "oled_line", 3, args[2], &x1) ||
        !jl_get_int(vm, "oled_line", 4, args[3], &y1)) return false;
    if (argc == 5 && !get_flag(vm, "oled_line", 5, args[4], &on)) return false;
    if (active_oled) oled_line(active_oled, (int)x0, (int)y0, (int)x1, (int)y1, on);
    return true;
}

static bool nat_oled_rect(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    if (argc < 4 || argc > 6) return jl_error(vm, "oled_rect(): expects 4 to 6 arguments (x, y, w, h[, fill[, on]])");
    int32_t x, y, w, h;
    bool fill = false, on = true;
    if (!jl_get_int(vm, "oled_rect", 1, args[0], &x) ||
        !jl_get_int(vm, "oled_rect", 2, args[1], &y) ||
        !jl_get_int(vm, "oled_rect", 3, args[2], &w) ||
        !jl_get_int(vm, "oled_rect", 4, args[3], &h)) return false;
    if (argc >= 5 && !get_flag(vm, "oled_rect", 5, args[4], &fill)) return false;
    if (argc == 6 && !get_flag(vm, "oled_rect", 6, args[5], &on)) return false;
    if (active_oled) oled_rect(active_oled, (int)x, (int)y, (int)w, (int)h, fill, on);
    return true;
}

static bool nat_oled_circle(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    if (argc < 3 || argc > 5) return jl_error(vm, "oled_circle(): expects 3 to 5 arguments (cx, cy, r[, fill[, on]])");
    int32_t cx, cy, r;
    bool fill = false, on = true;
    if (!jl_get_int(vm, "oled_circle", 1, args[0], &cx) ||
        !jl_get_int(vm, "oled_circle", 2, args[1], &cy) ||
        !jl_get_int(vm, "oled_circle", 3, args[2], &r)) return false;
    if (argc >= 4 && !get_flag(vm, "oled_circle", 4, args[3], &fill)) return false;
    if (argc == 5 && !get_flag(vm, "oled_circle", 5, args[4], &on)) return false;
    if (active_oled) oled_circle(active_oled, (int)cx, (int)cy, (int)r, fill, on);
    return true;
}

static bool nat_oled_text(JlVM *vm, int argc, Value *args, Value *out) {
    if (argc < 3 || argc > 5) return jl_error(vm, "oled_text(): expects 3 to 5 arguments (text, x, y[, scale[, on]])");
    char num[16];
    const char *s = NULL;
    uint32_t n = 0;
    if (args[0].type == VAL_INT) {
        n = (uint32_t)snprintf(num, sizeof num, "%d", (int)args[0].as.i);
        s = num;
    } else if (!jl_get_string(vm, args[0], &s, &n)) {
        return jl_error(vm, "oled_text(): text must be a string or an int, got %s", value_type_name(args[0]));
    }
    int32_t x, y, scale = 1;
    bool on = true;
    if (!jl_get_int(vm, "oled_text", 2, args[1], &x) ||
        !jl_get_int(vm, "oled_text", 3, args[2], &y)) return false;
    if (argc >= 4) {
        if (!jl_get_int(vm, "oled_text", 4, args[3], &scale)) return false;
        if (scale < 1 || scale > 4) return jl_error(vm, "oled_text(): scale must be 1..4, got %d", (int)scale);
    }
    if (argc == 5 && !get_flag(vm, "oled_text", 5, args[4], &on)) return false;
    int next_x = active_oled ? oled_text(active_oled, s, n, (int)x, (int)y, (int)scale, on) : (int)x;
    *out = int_val(next_x);
    return true;
}

static bool nat_oled_sprite(JlVM *vm, int argc, Value *args, Value *out) {
    (void)out;
    if (argc < 5 || argc > 6) return jl_error(vm, "oled_sprite(): expects 5 or 6 arguments (x, y, w, h, data[, on])");
    int32_t x, y, w, h;
    bool on = true;
    if (!jl_get_int(vm, "oled_sprite", 1, args[0], &x) ||
        !jl_get_int(vm, "oled_sprite", 2, args[1], &y) ||
        !jl_get_int(vm, "oled_sprite", 3, args[2], &w) ||
        !jl_get_int(vm, "oled_sprite", 4, args[3], &h)) return false;
    if (w <= 0 || h <= 0 || w > OLED_W || h > OLED_H) {
        return jl_error(vm, "oled_sprite(): size must be 1..%d by 1..%d", OLED_W, OLED_H);
    }
    if (argc == 6 && !get_flag(vm, "oled_sprite", 6, args[5], &on)) return false;
    if (!active_oled) return true;

    if (args[4].type == VAL_ARR) {
        Value *items = NULL;
        uint32_t count = 0;
        if (!jl_get_array(vm, "oled_sprite", args[4], &items, &count)) return false;
        if (count > 0 && items[0].type == VAL_ARR) {
            /* 2D array of rows of 0/1 pixels */
            for (int row = 0; row < h && (uint32_t)row < count; row++) {
                Value *ritems = NULL;
                uint32_t rlen = 0;
                if (!jl_get_array(vm, "oled_sprite", items[row], &ritems, &rlen)) return false;
                for (int col = 0; col < w && (uint32_t)col < rlen; col++) {
                    bool lit = (ritems[col].type == VAL_BOOL) ? (ritems[col].as.b != 0)
                             : (ritems[col].type == VAL_INT && ritems[col].as.i != 0);
                    if (lit) oled_pixel(active_oled, (int)x + col, (int)y + row, on);
                }
            }
            return true;
        }
        uint32_t stride = (uint32_t)((w + 7) / 8);
        uint32_t packed_need = stride * (uint32_t)h;
        if (count == packed_need) {
            uint8_t tmp[OLED_FB_SIZE];
            for (uint32_t i = 0; i < count; i++) {
                int32_t b;
                if (!jl_get_int(vm, "oled_sprite", 5, items[i], &b) || b < 0 || b > 255) {
                    return jl_error(vm, "oled_sprite(): packed sprite array bytes must be 0..255");
                }
                tmp[i] = (uint8_t)b;
            }
            oled_sprite_bits(active_oled, (int)x, (int)y, (int)w, (int)h, tmp, count, on);
            return true;
        }
        /* Flat 1D array of w*h pixels */
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                uint32_t idx = (uint32_t)(row * w + col);
                if (idx >= count) return true;
                bool lit = (items[idx].type == VAL_BOOL) ? (items[idx].as.b != 0)
                         : (items[idx].type == VAL_INT && items[idx].as.i != 0);
                if (lit) oled_pixel(active_oled, (int)x + col, (int)y + row, on);
            }
        }
        return true;
    }

    uint8_t *bytes = NULL;
    uint32_t nbytes = 0;
    if (!jl_get_bytes(vm, "oled_sprite", args[4], &bytes, &nbytes)) return false;
    oled_sprite_bits(active_oled, (int)x, (int)y, (int)w, (int)h, bytes, nbytes, on);
    return true;
}

static bool nat_oled_invert(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)out;
    bool inv = false;
    if (!get_flag(vm, "oled_invert", 1, args[0], &inv)) return false;
    if (active_oled) oled_invert(active_oled, inv);
    return true;
}

static bool nat_oled_contrast(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)out;
    int32_t c;
    if (!jl_get_int(vm, "oled_contrast", 1, args[0], &c)) return false;
    if (c < 0 || c > 255) return jl_error(vm, "oled_contrast(): contrast must be 0..255, got %d", (int)c);
    if (active_oled) oled_contrast(active_oled, (uint8_t)c);
    return true;
}

static bool nat_oled_show(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc; (void)args; (void)out;
    if (active_oled && active_show_hook) {
        return active_show_hook(vm, active_oled, active_show_ud);
    }
    return true;
}

void oled_register_natives(JlVM *vm, Oled *o, JlOledShowHook on_show, void *hook_ud) {
    active_oled = o;
    active_show_hook = on_show;
    active_show_ud = hook_ud;
    jl_register_native(vm, "oled_clear", nat_oled_clear, 0);
    jl_register_native(vm, "oled_width", nat_oled_width, 0);
    jl_register_native(vm, "oled_height", nat_oled_height, 0);
    jl_register_native(vm, "oled_pixel", nat_oled_pixel, -1);
    jl_register_native(vm, "oled_line", nat_oled_line, -1);
    jl_register_native(vm, "oled_rect", nat_oled_rect, -1);
    jl_register_native(vm, "oled_circle", nat_oled_circle, -1);
    jl_register_native(vm, "oled_text", nat_oled_text, -1);
    jl_register_native(vm, "oled_sprite", nat_oled_sprite, -1);
    jl_register_native(vm, "oled_invert", nat_oled_invert, 1);
    jl_register_native(vm, "oled_contrast", nat_oled_contrast, 1);
    jl_register_native(vm, "oled_show", nat_oled_show, 0);
}
