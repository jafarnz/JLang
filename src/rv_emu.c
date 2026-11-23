/*
 * rv_emu.c - an RV32IM emulator with a small simulated board.
 *
 * It runs machine code produced by rv_asm.c: fetch an instruction,
 * decode its fields, execute it, repeat. Memory is 1 MB of RAM at
 * address 0; addresses from 0x10000000 are peripherals (see rv.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rv.h"
#include "vcd.h"

typedef struct {
    const RvImage *img;
    const RvRunOptions *opt;
    uint8_t *mem;
    uint32_t x[32];
    uint32_t pc;
    uint64_t cycles;
    bool halted;
    int exit_code;
    /* board */
    uint32_t gpio_dir, gpio_pull, gpio_out;
    uint32_t now_ms;
    Panel panel;
    uint32_t led_xy;
} Cpu;

static void write_out(Cpu *c, const char *s, size_t n) {
    if (c->opt->out) c->opt->out(c->opt->ud, s, n);
}

static void fault(Cpu *c, const char *fmt, uint32_t value) {
    char msg[200], buf[320];
    snprintf(msg, sizeof msg, fmt, value);
    int line = rv_line_for(c->img, c->pc);
    int n = line > 0 ? snprintf(buf, sizeof buf, "runtime error (line %d): %s\n  at pc 0x%08x\n", line, msg, (unsigned)c->pc)
                     : snprintf(buf, sizeof buf, "runtime error: %s\n  at pc 0x%08x\n", msg, (unsigned)c->pc);
    if (c->opt->err) c->opt->err(c->opt->ud, buf, (size_t)n);
    c->halted = true;
    c->exit_code = 70;
}

static uint32_t gpio_in(Cpu *c) {
    uint32_t v = 0;
    for (int pin = 0; pin < 32; pin++) {
        uint32_t bit = 1u << pin;
        int level;
        if (c->gpio_dir & bit) level = (c->gpio_out & bit) != 0;
        else if (c->opt->pin_inputs[pin] >= 0) level = c->opt->pin_inputs[pin];
        else level = (c->gpio_pull & bit) != 0;
        if (level) v |= bit;
    }
    return v;
}

static bool mmio_read(Cpu *c, uint32_t addr, uint32_t *v) {
    switch (addr) {
    case RV_GPIO_DIR: *v = c->gpio_dir; return true;
    case RV_GPIO_PULL: *v = c->gpio_pull; return true;
    case RV_GPIO_OUT: *v = c->gpio_out; return true;
    case RV_GPIO_IN: *v = gpio_in(c); return true;
    case RV_TIMER_MS: *v = c->now_ms; return true;
    case RV_LED_W: *v = (uint32_t)c->panel.w; return true;
    case RV_LED_H: *v = (uint32_t)c->panel.h; return true;
    case RV_LED_XY: *v = c->led_xy; return true;
    case RV_LED_RGB:
    case RV_LED_SHOW:
    case RV_LED_CLEAR:
    case RV_UART_TX:
    case RV_TIMER_WAIT:
    case RV_EXIT: *v = 0; return true;
    default: return false;
    }
}

static bool mmio_write(Cpu *c, uint32_t addr, uint32_t v) {
    switch (addr) {
    case RV_UART_TX: {
        char ch = (char)(v & 0xFF);
        write_out(c, &ch, 1);
        return true;
    }
    case RV_GPIO_DIR: c->gpio_dir = v; return true;
    case RV_GPIO_PULL: c->gpio_pull = v; return true;
    case RV_GPIO_OUT: {
        /* Report output pins that change, like the VM's board simulator. */
        uint32_t changed = (c->gpio_out ^ v) & c->gpio_dir;
        c->gpio_out = v;
        for (int pin = 0; pin < 32; pin++) {
            if (!(changed & (1u << pin))) continue;
            char buf[64];
            int n = snprintf(buf, sizeof buf, "[sim %6ums] pin %d -> %s\n", (unsigned)c->now_ms, pin,
                             (v >> pin) & 1u ? "HIGH" : "LOW");
            write_out(c, buf, (size_t)n);
            if (c->opt->vcd) {
                char name[16];
                snprintf(name, sizeof name, "pin%d", pin);
                vcd_change(c->opt->vcd, c->now_ms, name, 1, (v >> pin) & 1u);
            }
        }
        return true;
    }
    case RV_TIMER_WAIT:
        c->now_ms += v;
        if (c->opt->max_ms && c->now_ms >= c->opt->max_ms && !c->halted) {
            char buf[160];
            int n = snprintf(buf, sizeof buf,
                             "stopped after %u seconds of simulated time. On a board the program would keep running\n",
                             (unsigned)(c->opt->max_ms / 1000));
            if (c->opt->err) c->opt->err(c->opt->ud, buf, (size_t)n);
            c->halted = true;
            c->exit_code = 70;
        }
        return true;
    case RV_LED_XY: c->led_xy = v; return true;
    case RV_LED_RGB:
        panel_set(&c->panel, (int16_t)(c->led_xy >> 16), (int16_t)(c->led_xy & 0xFFFF), v);
        return true;
    case RV_LED_SHOW: panel_show(&c->panel, c->now_ms, (JlPanelWriteFn)c->opt->out, c->opt->ud); return true;
    case RV_LED_CLEAR: panel_clear(&c->panel); return true;
    case RV_LED_W:
    case RV_LED_H: return true;
    case RV_EXIT:
        c->halted = true;
        c->exit_code = (int)v;
        return true;
    case RV_GPIO_IN:
    case RV_TIMER_MS: return true;
    default: return false;
    }
}

static uint32_t load(Cpu *c, uint32_t addr, int size) {
    if (addr >= 0x10000000u) {
        uint32_t v;
        if (size == 4 && mmio_read(c, addr, &v)) return v;
        if (size == 1 && mmio_read(c, addr & ~3u, &v)) return (v >> (8 * (addr & 3))) & 0xFF;
        fault(c, "read from 0x%08x, which is not memory or a peripheral", addr);
        return 0;
    }
    if (addr + (uint32_t)size > RV_RAM_SIZE || addr + (uint32_t)size < addr) {
        fault(c, "read from address 0x%08x, outside the 1 MB of RAM", addr);
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)c->mem[addr + (uint32_t)i] << (8 * i);
    return v;
}

static void store(Cpu *c, uint32_t addr, uint32_t v, int size) {
    if (addr >= 0x10000000u) {
        if (!mmio_write(c, addr, size == 4 ? v : v & (size == 1 ? 0xFFu : 0xFFFFu))) {
            fault(c, "write to 0x%08x, which is not memory or a peripheral", addr);
        }
        return;
    }
    if (addr + (uint32_t)size > RV_RAM_SIZE || addr + (uint32_t)size < addr) {
        fault(c, "write to address 0x%08x, outside the 1 MB of RAM", addr);
        return;
    }
    if (addr < c->img->text_size) {
        fault(c, "write to address 0x%08x, which is program code", addr);
        return;
    }
    for (int i = 0; i < size; i++) c->mem[addr + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

static int32_t sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

static void step(Cpu *c) {
    uint32_t pc = c->pc;
    if (pc & 3u || pc + 4 > c->img->text_size) {
        fault(c, "jumped to 0x%08x, which is not program code", pc);
        return;
    }
    uint32_t in = load(c, pc, 4);
    uint32_t op = in & 0x7F, rd = (in >> 7) & 31, f3 = (in >> 12) & 7;
    uint32_t rs1 = (in >> 15) & 31, rs2 = (in >> 20) & 31, f7 = in >> 25;
    uint32_t a = c->x[rs1], b = c->x[rs2];
    int32_t imm_i = sext(in >> 20, 12);
    uint32_t next = pc + 4;
    uint32_t result = 0;
    bool write = true;

    switch (op) {
    case 0x37: result = in & 0xFFFFF000u; break;          /* lui */
    case 0x17: result = pc + (in & 0xFFFFF000u); break;   /* auipc */
    case 0x6F: {                                          /* jal */
        uint32_t imm = ((in >> 31) << 20) | (((in >> 12) & 0xFF) << 12) | (((in >> 20) & 1) << 11) | (((in >> 21) & 0x3FF) << 1);
        result = next;
        next = pc + (uint32_t)sext(imm, 21);
        break;
    }
    case 0x67:                                            /* jalr */
        result = next;
        next = (a + (uint32_t)imm_i) & ~1u;
        break;
    case 0x63: {                                          /* branches */
        uint32_t imm = ((in >> 31) << 12) | (((in >> 7) & 1) << 11) | (((in >> 25) & 0x3F) << 5) | (((in >> 8) & 0xF) << 1);
        bool take;
        switch (f3) {
        case 0: take = a == b; break;
        case 1: take = a != b; break;
        case 4: take = (int32_t)a < (int32_t)b; break;
        case 5: take = (int32_t)a >= (int32_t)b; break;
        case 6: take = a < b; break;
        case 7: take = a >= b; break;
        default: fault(c, "illegal branch instruction 0x%08x", in); return;
        }
        if (take) next = pc + (uint32_t)sext(imm, 13);
        write = false;
        break;
    }
    case 0x03: {                                          /* loads */
        uint32_t addr = a + (uint32_t)imm_i;
        switch (f3) {
        case 0: result = (uint32_t)sext(load(c, addr, 1), 8); break;
        case 1: result = (uint32_t)sext(load(c, addr, 2), 16); break;
        case 2: result = load(c, addr, 4); break;
        case 4: result = load(c, addr, 1); break;
        case 5: result = load(c, addr, 2); break;
        default: fault(c, "illegal load instruction 0x%08x", in); return;
        }
        break;
    }
    case 0x23: {                                          /* stores */
        int32_t imm = sext(((in >> 25) << 5) | ((in >> 7) & 31), 12);
        uint32_t addr = a + (uint32_t)imm;
        if (f3 > 2) {
            fault(c, "illegal store instruction 0x%08x", in);
            return;
        }
        store(c, addr, b, 1 << f3);
        write = false;
        break;
    }
    case 0x13: {                                          /* immediate ALU */
        uint32_t sh = (in >> 20) & 31;
        switch (f3) {
        case 0: result = a + (uint32_t)imm_i; break;
        case 2: result = (int32_t)a < imm_i; break;
        case 3: result = a < (uint32_t)imm_i; break;
        case 4: result = a ^ (uint32_t)imm_i; break;
        case 6: result = a | (uint32_t)imm_i; break;
        case 7: result = a & (uint32_t)imm_i; break;
        case 1: result = a << sh; break;
        case 5: result = (f7 & 0x20) ? (uint32_t)((int32_t)a >> sh) : a >> sh; break;
        }
        break;
    }
    case 0x33:                                            /* register ALU */
        if (f7 == 1) {                                    /* M extension */
            switch (f3) {
            case 0: result = a * b; break;
            case 1: result = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
            case 2: result = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
            case 3: result = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
            case 4: result = b == 0 ? 0xFFFFFFFFu : (a == 0x80000000u && b == 0xFFFFFFFFu) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
            case 5: result = b == 0 ? 0xFFFFFFFFu : a / b; break;
            case 6: result = b == 0 ? a : (a == 0x80000000u && b == 0xFFFFFFFFu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
            case 7: result = b == 0 ? a : a % b; break;
            }
        } else {
            switch (f3) {
            case 0: result = (f7 & 0x20) ? a - b : a + b; break;
            case 1: result = a << (b & 31); break;
            case 2: result = (int32_t)a < (int32_t)b; break;
            case 3: result = a < b; break;
            case 4: result = a ^ b; break;
            case 5: result = (f7 & 0x20) ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
            case 6: result = a | b; break;
            case 7: result = a & b; break;
            }
        }
        break;
    case 0x0F: write = false; break;                      /* fence */
    case 0x73:                                            /* system */
        if (f3 == 0) {
            if (in == 0x00000073u && c->x[17] == 93) {    /* ecall exit(a0) */
                c->halted = true;
                c->exit_code = (int)c->x[10];
                return;
            }
            fault(c, in == 0x00100073u ? "ebreak instruction" : "unsupported ecall", in);
            return;
        } else {
            uint32_t csr = in >> 20;
            switch (csr) {
            case 0xC00: case 0xC02: result = (uint32_t)c->cycles; break;
            case 0xC80: case 0xC82: result = (uint32_t)(c->cycles >> 32); break;
            case 0xC01: result = c->now_ms; break;
            default: fault(c, "unsupported CSR 0x%03x", csr); return;
            }
        }
        break;
    default:
        fault(c, "illegal instruction 0x%08x", in);
        return;
    }
    if (c->halted) return;
    if (write && rd != 0) c->x[rd] = result;
    c->pc = next;
}

int rv_run(const RvImage *img, const RvRunOptions *opt, uint64_t *cycles_out) {
    Cpu c;
    memset(&c, 0, sizeof c);
    c.img = img;
    c.opt = opt;
    c.mem = calloc(1, RV_RAM_SIZE);
    if (c.mem == NULL) return 70;
    if (!panel_init(&c.panel, opt->panel_w ? opt->panel_w : PANEL_DEFAULT_W,
                    opt->panel_h ? opt->panel_h : PANEL_DEFAULT_H)) {
        free(c.mem);
        return 70;
    }
    c.panel.print = opt->frame_sink == NULL;
    c.panel.ansi = opt->ansi_frames;
    c.panel.sink = opt->frame_sink;
    c.panel.sink_data = opt->frame_data;
    memcpy(c.mem, img->image, img->size);

    uint32_t entry;
    if (!rv_symbol(img, "_start", &entry)) {
        const char *msg = "runtime error: the program has no _start label\n";
        if (opt->err) opt->err(opt->ud, msg, strlen(msg));
        free(c.mem);
        panel_free(&c.panel);
        return 70;
    }
    c.pc = entry;
    c.x[2] = RV_RAM_SIZE; /* stack pointer at the top of RAM */

    while (!c.halted) {
        step(&c);
        c.cycles++;
        if (opt->max_cycles && c.cycles >= opt->max_cycles && !c.halted) {
            char buf[160];
            int line = rv_line_for(img, c.pc);
            int n = snprintf(buf, sizeof buf, "runtime error (line %d): stopped after %llu instructions. "
                                              "Is there a loop that never ends?\n",
                             line, (unsigned long long)opt->max_cycles);
            if (opt->err) opt->err(opt->ud, buf, (size_t)n);
            c.exit_code = 70;
            break;
        }
    }
    if (cycles_out) *cycles_out = c.cycles;
    free(c.mem);
    panel_free(&c.panel);
    return c.exit_code;
}
