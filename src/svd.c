/*
 * svd.c - turn a chip's SVD file into JLang consts (jlang svd).
 *
 * Chip makers describe every peripheral register in an SVD file (System
 * View Description, an XML format from Arm): where it is, and what each
 * group of bits in it does. This prints them as JLang:
 *
 *   // GPIO: General Purpose Input/Output
 *   const GPIO_OUT = 0x60091004           // GPIO output register for GPIO0-31
 *   const GPIO_ENABLE = 0x60091020        // GPIO output enable register
 *   const GPIO_PIN0_PAD_DRIVER_BIT = 2    // set this bit to select the pad driver
 *   const GPIO_PIN0_INT_TYPE_HI = 9       // a field of bits 9 down to 7
 *   const GPIO_PIN0_INT_TYPE_LO = 7
 *
 * so registers read as mmio_read32(GPIO_OUT), and fields as
 * reg[GPIO_PIN0_INT_TYPE_HI..GPIO_PIN0_INT_TYPE_LO].
 */
#define _POSIX_C_SOURCE 200809L
#include "svd.h"
#include "port.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- a small XML reader, enough for SVD ---------------------------------- */

typedef struct XNode {
    char *tag;
    char *text;    /* text directly inside, trimmed */
    char *derived; /* the derivedFrom attribute */
    struct XNode *child, *last, *next;
} XNode;

static void xfree(XNode *n) {
    while (n) {
        XNode *next = n->next;
        xfree(n->child);
        free(n->tag);
        free(n->text);
        free(n->derived);
        free(n);
        n = next;
    }
}

static char *dup_range(const char *a, const char *b) {
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    char *s = malloc((size_t)(b - a) + 1);
    if (!s) return NULL;
    /* collapse runs of whitespace (descriptions span lines) */
    size_t n = 0;
    for (const char *p = a; p < b; p++) {
        if (isspace((unsigned char)*p)) {
            if (n && s[n - 1] != ' ') s[n++] = ' ';
        } else {
            s[n++] = *p;
        }
    }
    s[n] = '\0';
    return s;
}

/* Parses the elements from *p until the closing tag of the parent. */
static XNode *parse_children(const char **p, const char *end) {
    XNode *first = NULL, *last = NULL;
    while (*p < end) {
        const char *lt = memchr(*p, '<', (size_t)(end - *p));
        if (!lt) {
            *p = end;
            break;
        }
        *p = lt;
        if (strncmp(lt, "<!--", 4) == 0) {
            const char *c = strstr(lt, "-->");
            *p = c ? c + 3 : end;
            continue;
        }
        if (lt[1] == '?' || lt[1] == '!') {
            const char *c = strchr(lt, '>');
            *p = c ? c + 1 : end;
            continue;
        }
        if (lt[1] == '/') return first; /* the parent's closing tag */
        const char *gt = strchr(lt, '>');
        if (!gt) {
            *p = end;
            break;
        }
        XNode *n = calloc(1, sizeof *n);
        if (!n) break;
        const char *name_end = lt + 1;
        while (name_end < gt && !isspace((unsigned char)*name_end) && *name_end != '/') name_end++;
        n->tag = dup_range(lt + 1, name_end);
        const char *df = NULL;
        for (const char *q = lt; q + 13 <= gt && !df; q++)
            if (memcmp(q, "derivedFrom=\"", 13) == 0) df = q;
        if (df) {
            const char *q = strchr(df + 13, '"');
            if (q) n->derived = dup_range(df + 13, q);
        }
        if (last) last->next = n;
        else first = n;
        last = n;
        *p = gt + 1;
        if (gt[-1] == '/') continue; /* <tag/> */
        const char *text_start = *p;
        n->child = parse_children(p, end);
        if (!n->child) {
            const char *close = *p;
            n->text = dup_range(text_start, close);
        }
        /* skip </tag> */
        const char *c = strchr(*p, '>');
        *p = c ? c + 1 : end;
    }
    return first;
}

static XNode *find(const XNode *n, const char *tag) {
    for (XNode *c = n ? n->child : NULL; c; c = c->next)
        if (c->tag && strcmp(c->tag, tag) == 0) return c;
    return NULL;
}

static const char *text(const XNode *n, const char *tag) {
    XNode *c = find(n, tag);
    return c && c->text ? c->text : NULL;
}

static long long number(const char *s, long long dflt) {
    if (!s) return dflt;
    if (s[0] == '#') return strtoll(s + 1, NULL, 2); /* #0101 binary */
    return strtoll(s, NULL, 0);
}

/* ---- output ---------------------------------------------------------------- */

typedef struct {
    FILE *out;
    char **only;
    int nonly;
    int regs, fields;
} Gen;

/* A JLang name: letters, digits and underscores, upper case. */
static void ident(char *dst, size_t size, const char *a, const char *b, const char *c) {
    size_t n = 0;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3 && n + 1 < size; i++) {
        if (!parts[i] || !parts[i][0]) continue;
        if (n && n + 1 < size) dst[n++] = '_';
        for (const char *p = parts[i]; *p && n + 1 < size; p++) {
            char ch = (char)toupper((unsigned char)*p);
            if (!isalnum((unsigned char)ch)) ch = '_';
            if (ch == '_' && n && dst[n - 1] == '_') continue;
            dst[n++] = ch;
        }
        while (n && dst[n - 1] == '_') n--;
    }
    dst[n] = '\0';
}

/* The description, with %s (in arrays of registers) replaced by the index. */
static void comment(FILE *f, int col, const char *desc, const char *idx) {
    if (!desc || !desc[0]) {
        fputc('\n', f);
        return;
    }
    char text[512];
    size_t n = 0;
    for (const char *p = desc; *p && n + 1 < sizeof text; p++) {
        if (p[0] == '%' && p[1] == 's' && idx) {
            for (const char *q = idx; *q && n + 1 < sizeof text; q++) text[n++] = *q;
            p++;
        } else {
            text[n++] = *p;
        }
    }
    text[n] = '\0';
    fputc(' ', f);
    for (int i = col + 1; i < 44; i++) fputc(' ', f);
    fprintf(f, "// %.80s%s\n", text, n > 80 ? "..." : "");
}

static bool wanted(const Gen *g, const char *periph, const char *reg) {
    if (g->nonly == 0) return true;
    for (int i = 0; i < g->nonly; i++) {
        const char *o = g->only[i];
        const char *dot = strchr(o, '.');
        size_t plen = dot ? (size_t)(dot - o) : strlen(o);
        if (strlen(periph) != plen || jl_strncasecmp(periph, o, plen) != 0) continue;
        if (!dot || !reg) return true;
        if (jl_strncasecmp(reg, dot + 1, strlen(reg) + 1) == 0) return true;
    }
    return false;
}

static void emit_register(Gen *g, const char *periph, const XNode *reg, const char *name, uint32_t addr,
                          const char *idx) {
    if (!wanted(g, periph, name)) return;
    char id[160];
    ident(id, sizeof id, periph, name, NULL);
    int col = fprintf(g->out, "const %s = 0x%08X", id, (unsigned)addr);
    comment(g->out, col, text(reg, "description"), idx);
    g->regs++;
    XNode *fields = find(reg, "fields");
    for (XNode *f = fields ? fields->child : NULL; f; f = f->next) {
        if (!f->tag || strcmp(f->tag, "field") != 0) continue;
        long long lo = -1, width = -1;
        if (text(f, "bitOffset")) {
            lo = number(text(f, "bitOffset"), 0);
            width = number(text(f, "bitWidth"), 1);
        } else if (text(f, "lsb")) {
            lo = number(text(f, "lsb"), 0);
            width = number(text(f, "msb"), lo) - lo + 1;
        } else if (text(f, "bitRange")) { /* [msb:lsb] */
            long long msb = 0, lsb = 0;
            if (sscanf(text(f, "bitRange"), "[%lld:%lld]", &msb, &lsb) == 2) {
                lo = lsb;
                width = msb - lsb + 1;
            }
        }
        if (lo < 0 || width < 1 || lo + width > 32) continue;
        if (width == 32) continue; /* the field is the whole register */
        char fid[220];
        ident(fid, sizeof fid, periph, name, text(f, "name"));
        if (width == 1) {
            col = fprintf(g->out, "const %s_BIT = %lld", fid, lo);
            comment(g->out, col, text(f, "description"), idx);
        } else {
            col = fprintf(g->out, "const %s_HI = %lld", fid, lo + width - 1);
            comment(g->out, col, text(f, "description"), idx);
            fprintf(g->out, "const %s_LO = %lld\n", fid, lo);
        }
        g->fields++;
    }
}

/* Registers, expanding arrays (dim) and clusters. */
static void emit_registers(Gen *g, const char *periph, const XNode *regs, uint32_t base, const char *prefix,
                           const char *outer_idx) {
    for (XNode *r = regs ? regs->child : NULL; r; r = r->next) {
        if (!r->tag) continue;
        bool cluster = strcmp(r->tag, "cluster") == 0;
        if (!cluster && strcmp(r->tag, "register") != 0) continue;
        const char *raw = text(r, "name");
        if (!raw) continue;
        long long dim = number(text(r, "dim"), 1);
        long long step = number(text(r, "dimIncrement"), 4);
        uint32_t off = (uint32_t)number(text(r, "addressOffset"), 0);
        if (dim < 1 || dim > 1024) dim = 1;
        for (long long i = 0; i < dim; i++) {
            char name[128];
            char here[24];
            snprintf(here, sizeof here, "%lld", i);
            const char *idx = dim > 1 || strstr(raw, "%s") ? here : outer_idx;
            const char *pct = strstr(raw, "%s");
            if (pct) {
                /* IV_MEM[%s] -> IV_MEM0, CH%s_CONF -> CH0_CONF */
                size_t pre = (size_t)(pct - raw);
                if (pre > 0 && raw[pre - 1] == '[') pre--;
                const char *post = pct + 2;
                if (*post == ']') post++;
                snprintf(name, sizeof name, "%s%.*s%s%s", prefix, (int)pre, raw, here, post);
            } else {
                snprintf(name, sizeof name, "%s%s", prefix, raw);
            }
            uint32_t addr = base + off + (uint32_t)(i * step);
            if (cluster) {
                char sub[140];
                snprintf(sub, sizeof sub, "%s_", name);
                emit_registers(g, periph, r, addr, sub, idx);
            } else {
                emit_register(g, periph, r, name, addr, idx);
            }
        }
    }
}

bool svd_to_jlang(const char *xml, const char *source_name, char **only, int nonly, FILE *out, char *err, size_t errlen) {
    const char *p = xml;
    XNode *root = parse_children(&p, xml + strlen(xml));
    XNode *device = NULL;
    for (XNode *n = root; n; n = n->next)
        if (n->tag && strcmp(n->tag, "device") == 0) device = n;
    XNode *periphs = find(device, "peripherals");
    if (!periphs) {
        snprintf(err, errlen, "%s doesn't look like an SVD file (no <device> with <peripherals>)", source_name);
        xfree(root);
        return false;
    }
    Gen g = {out, only, nonly, 0, 0};
    const char *chip = text(device, "name");
    fprintf(out, "// Registers of the %s, made by jlang svd from %s.\n", chip ? chip : "chip", source_name);
    fprintf(out, "// Each register is an address: read it with mmio_read32(NAME) and write it with\n");
    fprintf(out, "// mmio_write32(NAME, value). A field of bits is NAME_HI..NAME_LO, as in\n");
    fprintf(out, "// reg[NAME_HI..NAME_LO], and a one-bit field is NAME_BIT, as in reg[NAME_BIT].\n");
    for (XNode *per = periphs->child; per; per = per->next) {
        if (!per->tag || strcmp(per->tag, "peripheral") != 0) continue;
        const char *name = text(per, "name");
        if (!name || !wanted(&g, name, NULL)) continue;
        uint32_t base = (uint32_t)number(text(per, "baseAddress"), 0);
        const XNode *regs = find(per, "registers");
        const char *desc = text(per, "description");
        if (!regs && per->derived) {
            /* UART1 is laid out like UART0, at another address */
            for (XNode *o = periphs->child; o; o = o->next)
                if (o->tag && text(o, "name") && strcmp(text(o, "name"), per->derived) == 0) {
                    regs = find(o, "registers");
                    if (!desc) desc = text(o, "description");
                }
        }
        fprintf(out, "\n// %s%s%s\n", name, desc ? ": " : "", desc ? desc : "");
        emit_registers(&g, name, regs, base, "", NULL);
    }
    fprintf(out, "\n// %d registers and %d fields.\n", g.regs, g.fields);
    xfree(root);
    if (g.regs == 0 && nonly > 0) {
        snprintf(err, errlen, "nothing in %s matches --only (use peripheral names like GPIO, or GPIO.OUT)", source_name);
        return false;
    }
    return true;
}
