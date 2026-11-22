/*
 * panel.c - see panel.h.
 */
#include "panel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool panel_init(Panel *p, int w, int h) {
    memset(p, 0, sizeof *p);
    p->w = w < 1 ? 1 : w > PANEL_MAX_W ? PANEL_MAX_W : w;
    p->h = h < 1 ? 1 : h > PANEL_MAX_H ? PANEL_MAX_H : h;
    p->rgb = calloc((size_t)(p->w * p->h), sizeof *p->rgb);
    return p->rgb != NULL;
}

void panel_free(Panel *p) {
    free(p->rgb);
    p->rgb = NULL;
}

void panel_set(Panel *p, int x, int y, uint32_t rgb) {
    if (p->rgb == NULL || x < 0 || y < 0 || x >= p->w || y >= p->h) return;
    p->rgb[y * p->w + x] = rgb & 0xFFFFFF;
}

void panel_clear(Panel *p) {
    if (p->rgb) memset(p->rgb, 0, (size_t)(p->w * p->h) * sizeof *p->rgb);
}

void panel_show(Panel *p, uint32_t ms, JlPanelWriteFn write, void *write_data) {
    if (p->rgb == NULL) return;
    p->frames++;
    if (p->sink != NULL) p->sink(p->sink_data, ms, p->w, p->h, p->rgb);
    if (!p->print || write == NULL) return;

    char line[PANEL_MAX_W * 24 + 64];
    int n;
    if (p->ansi && p->frames > 1) {
        /* move back up over the previous frame so it is redrawn in place */
        n = snprintf(line, sizeof line, "\x1b[%dA", p->h + 1);
        write(write_data, line, (size_t)n);
    }
    n = snprintf(line, sizeof line, "[sim %6ums] frame %u\n", (unsigned)ms, (unsigned)p->frames);
    write(write_data, line, (size_t)n);
    for (int y = 0; y < p->h; y++) {
        n = 0;
        for (int x = 0; x < p->w; x++) {
            uint32_t c = p->rgb[y * p->w + x];
            if (p->ansi) {
                if (c) n += snprintf(line + n, sizeof line - (size_t)n, "\x1b[38;2;%u;%u;%um\xe2\x96\x88",
                                     (unsigned)(c >> 16), (unsigned)(c >> 8 & 0xFF), (unsigned)(c & 0xFF));
                else n += snprintf(line + n, sizeof line - (size_t)n, "\x1b[38;2;60;60;60m\xc2\xb7");
            } else {
                line[n++] = c ? '#' : '.';
            }
        }
        if (p->ansi) n += snprintf(line + n, sizeof line - (size_t)n, "\x1b[0m");
        line[n++] = '\n';
        write(write_data, line, (size_t)n);
    }
}
