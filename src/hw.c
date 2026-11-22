/*
 * hw.c - colours and notes for the beginner hardware built-ins.
 */
#include "hw.h"

#include <string.h>

static const struct {
    const char *name;
    uint32_t rgb;
} colors[] = {
    {"off", 0x000000},    {"black", 0x000000}, {"white", 0xFFFFFF},  {"red", 0xFF0000},
    {"green", 0x00FF00},  {"blue", 0x0000FF},  {"yellow", 0xFFFF00}, {"cyan", 0x00FFFF},
    {"magenta", 0xFF00FF}, {"orange", 0xFF8000}, {"purple", 0x8000FF}, {"pink", 0xFF4080},
    {"warm", 0xFFB060},
};

static bool same(const char *a, uint32_t n, const char *b) {
    if (strlen(b) != n) return false;
    for (uint32_t i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != b[i]) return false;
    }
    return true;
}

bool jl_hw_color(JlVM *vm, const char *fn, int arg_no, Value v, uint32_t *rgb) {
    if (v.type == VAL_INT) {
        if (v.as.i < 0 || v.as.i > 0xFFFFFF)
            return jl_error(vm, "%s(): a colour is 0xRRGGBB, from 0x000000 to 0xFFFFFF, but got %d", fn, (int)v.as.i);
        *rgb = (uint32_t)v.as.i;
        return true;
    }
    const char *s;
    uint32_t n;
    if (v.type == VAL_STR && jl_get_string(vm, v, &s, &n)) {
        for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
            if (same(s, n, colors[i].name)) {
                *rgb = colors[i].rgb;
                return true;
            }
        }
        return jl_error(vm, "%s(): '%.*s' isn't a colour I know. Use red, green, blue, yellow, cyan, magenta, "
                            "orange, purple, pink, white, warm or off, or a number like 0xFF8000",
                        fn, (int)n, s);
    }
    return jl_error(vm, "%s(): argument %d must be a colour like \"red\" or 0xFF8000, got %s", fn, arg_no,
                    value_type_name(v));
}

const char *jl_hw_color_name(uint32_t rgb) {
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++)
        if (colors[i].rgb == rgb) return colors[i].name;
    return NULL;
}

/* Note frequencies in octave 4, from C to B, times 100. */
static const int32_t octave4[12] = {26163, 27718, 29366, 31113, 32963, 34923,
                                    36999, 39200, 41530, 44000, 46616, 49388};

bool jl_hw_note(JlVM *vm, const char *fn, int arg_no, Value v, int32_t *hz) {
    if (v.type == VAL_INT) {
        if (v.as.i != 0 && (v.as.i < 20 || v.as.i > 20000))
            return jl_error(vm, "%s(): %d Hz can't be played. Use 20 to 20000 Hz, or 0 for silence", fn, (int)v.as.i);
        *hz = v.as.i;
        return true;
    }
    const char *s;
    uint32_t n;
    if (v.type != VAL_STR || !jl_get_string(vm, v, &s, &n))
        return jl_error(vm, "%s(): argument %d must be a frequency like 440 or a note like \"A4\", got %s", fn, arg_no,
                        value_type_name(v));
    if (same(s, n, "rest")) {
        *hz = 0;
        return true;
    }
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G */
    uint32_t i = 0;
    char letter = n > 0 ? s[0] : 0;
    if (letter >= 'a' && letter <= 'g') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'G') goto bad;
    int semis = base[letter - 'A'];
    i = 1;
    if (i < n && s[i] == '#') semis++, i++;
    else if (i < n && s[i] == 'b') semis--, i++;
    if (i + 1 != n || s[i] < '0' || s[i] > '8') goto bad;
    int octave = s[i] - '0';
    if (semis < 0) semis += 12, octave--;
    if (semis > 11) semis -= 12, octave++;
    int64_t f = octave4[semis];
    for (int o = octave; o > 4; o--) f *= 2;
    for (int o = octave; o < 4; o++) f /= 2;
    *hz = (int32_t)((f + 50) / 100);
    if (*hz < 20) goto bad;
    return true;
bad:
    return jl_error(vm, "%s(): '%.*s' isn't a note. Write a letter, an optional # or b, and an octave, like "
                        "\"C4\", \"F#5\" or \"Bb3\", or \"rest\" for silence",
                    fn, (int)n, s);
}
