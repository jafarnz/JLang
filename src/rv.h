/*
 * rv.h - JLang's RISC-V toolchain: assembler, emulator and code generator.
 *
 * JLang can compile a program to RV32IM machine code instead of running
 * it on the bytecode VM. The pipeline is:
 *
 *   source -> bytecode (compiler.c) -> RISC-V assembly (rv_codegen.c)
 *          -> machine code (rv_asm.c) -> emulator (rv_emu.c) or a real chip
 *
 * The assembly is plain GNU syntax, so it also builds with the standard
 * riscv-*-elf toolchain for real boards.
 *
 * The simulated board used by the emulator (and by the VM's mmio_* built-ins)
 * has this memory map:
 */
#ifndef JL_RV_H
#define JL_RV_H

#include "panel.h"
#include "common.h"

#define RV_RAM_SIZE (1u << 20) /* 1 MB of RAM starting at address 0 */

#define RV_UART_TX    0x10000000u /* write a byte to print it */
#define RV_GPIO_DIR   0x10001000u /* bit n = 1: pin n is an output */
#define RV_GPIO_PULL  0x10001004u /* bit n = 1: pin n has a pull-up */
#define RV_GPIO_OUT   0x10001008u /* bit n = level driven on pin n */
#define RV_GPIO_IN    0x1000100Cu /* bit n = level read from pin n */
#define RV_TIMER_MS   0x10002000u /* read: milliseconds since start */
#define RV_TIMER_WAIT 0x10002004u /* write n: wait n milliseconds */
#define RV_EXIT       0x10003000u /* write a code to stop the program */
#define RV_LED_XY     0x10004000u /* write x << 16 | y (each 16-bit, signed) */
#define RV_LED_RGB    0x10004004u /* write 0xRRGGBB: sets the pixel at LED_XY */
#define RV_LED_SHOW   0x10004008u /* write: show the frame */
#define RV_LED_CLEAR  0x1000400Cu /* write: turn every pixel off */
#define RV_LED_W      0x10004010u /* read: panel width */
#define RV_LED_H      0x10004014u /* read: panel height */

/* ---- assembler -------------------------------------------------------- */

typedef struct {
    char name[64];
    uint32_t addr;
} RvSymbol;

typedef struct {
    uint32_t addr;
    int line; /* JLang source line, from "# @line N" comments */
} RvLine;

/* A loop whose count is known, from "# @bound N" before its jump back. */
typedef struct {
    uint32_t addr; /* the jump back to the start of the loop */
    int count;     /* how many times the loop body runs */
} RvBound;

typedef struct {
    uint8_t *image;     /* memory contents, starting at address 0 */
    uint32_t size;      /* bytes used, including .bss */
    uint32_t text_size;
    RvSymbol *syms;
    int nsyms;
    RvLine *lines;
    int nlines;
    RvBound *bounds;
    int nbounds;
    char *listing;      /* optional: "addr  bytes  source" lines */
    char error[320];
} RvImage;

/* Assembles GNU-style RV32IM source. With allow_undefined, unknown
 * symbols resolve to 0 (useful for listings of hardware builds). */
bool rv_assemble(const char *src, RvImage *out, bool want_listing, bool allow_undefined);
void rv_image_free(RvImage *img);
bool rv_symbol(const RvImage *img, const char *name, uint32_t *addr);
int rv_line_for(const RvImage *img, uint32_t addr);

/* ---- emulator --------------------------------------------------------- */

typedef void (*RvWriteFn)(void *ud, const char *data, size_t len);

typedef struct {
    uint64_t max_cycles;     /* 0 = no limit */
    int pin_inputs[32];      /* value an input pin reads, -1 = default */
    RvWriteFn out;
    RvWriteFn err;
    void *ud;
    int panel_w, panel_h;    /* LED panel size; 0 = 60 x 7 */
    bool ansi_frames;        /* print frames in colour, redrawn in place */
    JlFrameFn frame_sink;    /* if set, frames go here instead of being printed */
    void *frame_data;
    uint32_t max_ms;         /* stop once this much time has passed, 0 = never */
    struct Vcd *vcd;         /* record pin changes for a waveform viewer, or NULL */
    /* Called after every instruction, for the pipeline view. */
    void (*trace)(void *trace_ud, uint32_t pc, uint32_t insn, uint32_t next_pc);
    void *trace_ud;
} RvRunOptions;

/* Runs an assembled image from symbol `_start`. Returns the exit code
 * written to RV_EXIT, or 70 after a fault (reported through err). */
int rv_run(const RvImage *img, const RvRunOptions *opt, uint64_t *cycles_out);

/* ---- pipeline view (rv_pipe.c) ----------------------------------------- */

/* Writes one RV32IM instruction as assembly, like "addi sp, sp, -16". */
void rv_disasm(uint32_t insn, uint32_t pc, char *out, size_t size);

/* A classic 5-stage pipeline (fetch, decode, execute, memory, write back)
 * with forwarding, fed with the instructions a program really ran. */
typedef struct RvPipe RvPipe;
/* The diagram starts at the first instruction of the program's own code. */
RvPipe *rv_pipe_new(const RvImage *img, int diagram_rows);
void rv_pipe_free(RvPipe *p);
void rv_pipe_step(void *pipe, uint32_t pc, uint32_t insn, uint32_t next_pc);
void rv_pipe_report(const RvPipe *p, const RvImage *img, RvWriteFn out, void *ud);

/* ---- worst-case timing (rv_wcet.c) -------------------------------------- */

/* The most cycles the script and each event handler can take. */
void rv_wcet_report(const RvImage *img, uint32_t mhz, RvWriteFn out, void *ud);

#endif
