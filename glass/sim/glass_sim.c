/*
 * glass_sim.c - runs Glass on a computer with a simulated board.
 *
 * The simulated board decodes what glass.jlang sends: the bits it clocks
 * into the MAX7219 chain on the LED pins, and the I2C messages it sends to
 * the SH1106/SSD1306 screen. So a picture from the simulator shows what
 * the drivers would make the real hardware display. Time is virtual:
 * delay() returns at once and advances millis().
 *
 *   glass_sim [options] glass.jlang program.jlang
 *
 *   --knob N        the knob's position, 0 (paused) to 4095 (max speed)
 *   --press MS      press the button for 30 ms every MS milliseconds
 *   --snap MS       save a picture at this virtual time (repeatable)
 *   --out DIR       where pictures go (default .), as snap-<ms>.ppm
 *   --until MS      stop at this virtual time (default 20000)
 *   --ssd1306       simulate a 0.96" SSD1306 instead of the 1.3" SH1106
 *   --stats         print how much work the display did per frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glass.h"

/* The wiring glass.jlang uses by default. */
enum { BUTTON = 1, LED_CS = 2, LED_CLK = 19, LED_DIN = 18, OLED_ADDR = 0x3C };

static struct {
    long long now, until, press_every;
    int knob;
    long long snaps[64];
    int nsnaps, next_snap;
    const char *out_dir;
    int col_offset; /* 2 for SH1106, 0 for SSD1306 */

    int pins[32];
    /* four MAX7219s: chip 0 is nearest DIN and sits on the right */
    uint16_t shift[4];
    uint8_t rows[4][8], intensity;
    bool on;
    /* the screen controller's memory: 8 pages of 132 columns */
    uint8_t gram[8][132];
    int page, col;
    bool oled_on;
    long frames, i2c_bytes;
} s;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "glass_sim: can't open %s\n", path);
        exit(66);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) exit(66);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* ---- pictures ------------------------------------------------------------ */

/* LED (x, y) of the 32x8 matrix as seen from the front, x from the left. */
static bool led_at(int x, int y) {
    int chip = 3 - x / 8; /* chip 0 is on the right */
    return s.on && (s.rows[chip][y] >> (7 - x % 8)) & 1;
}

static bool pixel_at(int x, int y) {
    return s.oled_on && (s.gram[y / 8][x + s.col_offset] >> (y % 8)) & 1;
}

static void snapshot(long long t) {
    enum { W = 400, H = 330, LS = 12, OS = 3 };
    static uint8_t img[H][W][3];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) img[y][x][0] = img[y][x][1] = img[y][x][2] = 24;
    /* the LED matrix, 12 px per LED */
    for (int ly = 0; ly < 8; ly++)
        for (int lx = 0; lx < 32; lx++)
            for (int dy = 0; dy < LS; dy++)
                for (int dx = 0; dx < LS; dx++) {
                    int cx = dx - LS / 2, cy = dy - LS / 2;
                    if (cx * cx + cy * cy > 20) continue;
                    uint8_t *p = img[8 + ly * LS + dy][8 + lx * LS + dx];
                    bool lit = led_at(lx, ly);
                    p[0] = lit ? 255 : 60;
                    p[1] = lit ? 40 : 20;
                    p[2] = lit ? 30 : 20;
                }
    /* the screen, 3 px per pixel */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
            for (int dy = 0; dy < OS; dy++)
                for (int dx = 0; dx < OS; dx++) {
                    uint8_t *p = img[120 + y * OS + dy][8 + x * OS + dx];
                    uint8_t v = pixel_at(x, y) ? 235 : 0;
                    p[0] = p[1] = p[2] = v;
                }
    char path[512];
    snprintf(path, sizeof path, "%s/snap-%lld.ppm", s.out_dir, t);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, sizeof img, f);
    fclose(f);

    /* and a text version */
    printf("---- %lld ms ----\n", t);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 32; x++) putchar(led_at(x, y) ? '#' : '.');
        putchar('\n');
    }
    putchar('\n');
    for (int y = 0; y < 64; y += 2) {
        for (int x = 0; x < 128; x++) {
            bool a = pixel_at(x, y), b = pixel_at(x, y + 1);
            fputs(a && b ? "█" : a ? "▀" : b ? "▄" : " ", stdout);
        }
        putchar('\n');
    }
}

static bool advance(JlVM *vm, long long ms) {
    long long target = s.now + ms;
    while (s.next_snap < s.nsnaps && s.snaps[s.next_snap] <= target) {
        snapshot(s.snaps[s.next_snap]);
        s.next_snap++;
    }
    s.now = target;
    if (s.now >= s.until) return jl_error(vm, "the simulation ended at %lld ms", s.until);
    return true;
}

/* ---- the MAX7219 chain ---------------------------------------------------- */

static void max_clock(void) {
    /* each chip's top bit moves into the next chip down the chain */
    for (int i = 3; i > 0; i--) s.shift[i] = (uint16_t)(s.shift[i] << 1 | s.shift[i - 1] >> 15);
    s.shift[0] = (uint16_t)(s.shift[0] << 1 | (s.pins[LED_DIN] & 1));
}

static void max_latch(void) {
    for (int i = 0; i < 4; i++) {
        int reg = (s.shift[i] >> 8) & 0xF, data = s.shift[i] & 0xFF;
        if (reg >= 1 && reg <= 8) s.rows[i][reg - 1] = (uint8_t)data;
        else if (reg == 0x0C) s.on = data & 1;
        else if (reg == 0x0A) s.intensity = (uint8_t)data;
    }
}

/* ---- built-ins ------------------------------------------------------------- */

static bool get_pin(JlVM *vm, const char *fn, Value v, int32_t *pin) {
    if (!jl_get_int(vm, fn, 1, v, pin)) return false;
    if (*pin < 0 || *pin >= 32) return jl_error(vm, "%s(): there is no GPIO %d", fn, (int)*pin);
    return true;
}

static bool b_pin_mode(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin;
    return get_pin(vm, "pin_mode", args[0], &pin);
}

static bool b_pin_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t pin, level;
    if (!get_pin(vm, "pin_write", args[0], &pin) || !jl_get_int(vm, "pin_write", 2, args[1], &level)) return false;
    int old = s.pins[pin];
    s.pins[pin] = level != 0;
    if (pin == LED_CLK && !old && s.pins[pin]) max_clock();
    if (pin == LED_CS && !old && s.pins[pin]) max_latch();
    return true;
}

static bool b_pin_read(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc;
    int32_t pin;
    if (!get_pin(vm, "pin_read", args[0], &pin)) return false;
    int level = 1;
    if (pin == BUTTON && s.press_every > 0 && s.now % s.press_every >= s.press_every - 30) level = 0;
    *out = int_val(level);
    return true;
}

static bool b_delay(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t ms;
    if (!jl_get_int(vm, "delay", 1, args[0], &ms)) return false;
    return advance(vm, ms > 0 ? ms : 0);
}

static bool b_millis(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val((int32_t)s.now);
    return true;
}

static bool b_knob(JlVM *vm, int argc, Value *args, Value *out) {
    (void)vm, (void)argc, (void)args;
    *out = int_val(s.knob);
    return true;
}

/* Multi-byte SH1106/SSD1306 commands: how many argument bytes follow. */
static int cmd_args(uint8_t c) {
    switch (c) {
    case 0x81: case 0x8D: case 0xA8: case 0xAD: case 0xD3: case 0xD5:
    case 0xD9: case 0xDA: case 0xDB: case 0x20: return 1;
    case 0x21: case 0x22: return 2;
    default: return 0;
    }
}

static bool b_i2c_write(JlVM *vm, int argc, Value *args, Value *out) {
    (void)argc, (void)out;
    int32_t addr, control, off, n;
    uint8_t *data;
    uint32_t len;
    if (!jl_get_int(vm, "i2c_write", 1, args[0], &addr) || !jl_get_int(vm, "i2c_write", 2, args[1], &control) ||
        !jl_get_bytes(vm, "i2c_write", args[2], &data, &len) || !jl_get_int(vm, "i2c_write", 4, args[3], &off) ||
        !jl_get_int(vm, "i2c_write", 5, args[4], &n))
        return false;
    if (off < 0 || n < 0 || (uint32_t)off + (uint32_t)n > len)
        return jl_error(vm, "i2c_write(): bytes %d..%d are outside the %u-byte buffer", (int)off, (int)(off + n - 1),
                        (unsigned)len);
    if (addr != OLED_ADDR) return jl_error(vm, "i2c_write(): no device answered at address 0x%02X", (unsigned)addr);
    data += off;
    s.i2c_bytes += n + 1;
    if (control == 0x40) {
        for (int i = 0; i < n; i++)
            if (s.col < 132) s.gram[s.page][s.col++] = data[i];
    } else if (control == 0x00) {
        for (int i = 0; i < n; i++) {
            uint8_t c = data[i];
            if (c >= 0xB0 && c <= 0xB7) s.page = c & 7;
            else if (c <= 0x0F) s.col = (s.col & 0xF0) | c;
            else if (c >= 0x10 && c <= 0x1F) s.col = (s.col & 0x0F) | (c & 0x0F) << 4;
            else if (c == 0xAF) s.oled_on = true;
            else if (c == 0xAE) s.oled_on = false;
            else i += cmd_args(c);
        }
    } else {
        return jl_error(vm, "i2c_write(): the control byte must be 0x00 or 0x40, got 0x%02X", (unsigned)control);
    }
    return true;
}

static void echo(void *ud, const char *data, size_t len) {
    (void)ud;
    fwrite(data, 1, len, stderr);
}

static char *sim_read_file(void *ud, const char *path) {
    (void)ud;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    s.until = 20000;
    s.out_dir = ".";
    s.col_offset = 2;
    const char *files[2];
    bool stats = false;
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--knob") && i + 1 < argc) s.knob = atoi(argv[++i]);
        else if (!strcmp(a, "--press") && i + 1 < argc) s.press_every = atoll(argv[++i]);
        else if (!strcmp(a, "--until") && i + 1 < argc) s.until = atoll(argv[++i]);
        else if (!strcmp(a, "--out") && i + 1 < argc) s.out_dir = argv[++i];
        else if (!strcmp(a, "--snap") && i + 1 < argc && s.nsnaps < 64) s.snaps[s.nsnaps++] = atoll(argv[++i]);
        else if (!strcmp(a, "--ssd1306")) s.col_offset = 0;
        else if (!strcmp(a, "--stats")) stats = true;
        else if (a[0] != '-' && nfiles < 2) files[nfiles++] = a;
        else {
            fprintf(stderr, "usage: glass_sim [--knob N] [--press MS] [--snap MS]... [--out DIR] [--until MS] "
                            "[--ssd1306] glass.jlang [program.jlang]\n");
            return 64;
        }
    }
    if (nfiles < 1) {
        fprintf(stderr, "glass_sim: give a Glass app or display program (and optional program to show)\n");
        return 64;
    }

    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = 16 * 1024;
    JlVM *display = jl_new(&cfg);
    jl_set_file_reader(display, files[0], sim_read_file, NULL);
    jl_register_native(display, "pin_mode", b_pin_mode, 2);
    jl_register_native(display, "pin_write", b_pin_write, 2);
    jl_register_native(display, "pin_read", b_pin_read, 1);
    jl_register_native(display, "delay", b_delay, 1);
    jl_register_native(display, "millis", b_millis, 0);
    jl_register_native(display, "knob", b_knob, 0);
    jl_register_native(display, "i2c_write", b_i2c_write, 5);
    glass_register(display);

    char *glass = read_file(files[0]);
    JlResult r = jl_run_source(display, glass);
    if (r != JL_OK && !(nfiles == 1 && s.now >= s.until)) {
        jl_free(display);
        free(glass);
        return 70;
    }
    if (nfiles == 2) {
        char *program = read_file(files[1]);
        /* Like the firmware: run the program again each time it ends. */
        while (s.now < s.until) {
            long long before = s.now;
            glass_run(display, program, 16 * 1024, echo);
            if (s.now == before && !advance(display, 1000)) break;
        }
        s.frames = (long)glass_ticks();
        if (stats && s.frames > 0)
            fprintf(stderr, "%ld frames, %llu display instructions and %ld I2C bytes per frame\n", s.frames,
                    (unsigned long long)(jl_cycles(display) / (uint64_t)s.frames), s.i2c_bytes / s.frames);
        free(program);
    }
    /* pictures requested after the end */
    while (s.next_snap < s.nsnaps) snapshot(s.snaps[s.next_snap++]);
    jl_free(display);
    free(glass);
    return 0;
}
