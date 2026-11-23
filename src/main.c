/*
 * main.c - the `jlang` command-line tool.
 *
 *   jlang new blinky        create a starter project
 *   jlang run main.jlang    run a script on the simulated board
 *   jlang check main.jlang  only check for errors (fast; good for editors and AI agents)
 *   jlang xray main.jlang   run and show every instruction and the stack
 *   jlang dis main.jlang    show the bytecode without running
 *   jlang repl              type JLang and see results immediately
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"

#include "board_sim.h"
#include "jlang.h"
#include "rv.h"
#include "rv_codegen.h"
#include "svd.h"

enum {
    EXIT_USAGE = 64,
    EXIT_COMPILE = 65,
    EXIT_NOFILE = 66,
    EXIT_RUNTIME = 70,
};

#define MAX_EMITS 64

typedef struct {
    const char *command;
    const char *file;
    uint32_t heap;
    uint64_t max_cycles;
    bool stats;
    bool no_leak_check;
    const char *out_path;
    bool listing;
    bool print_asm;
    const char *bin_path;
    const char *vcd_path;
    bool pipeline_on;       /* jlang emu --pipeline */
    bool wcet;              /* jlang wcet */
    const char *only;       /* jlang svd --only GPIO,LEDC */
    uint32_t mhz;           /* clock speed for jlang wcet */
    int pipeline_rows;      /* rows of its diagram */
    int run_for_s;          /* how long timers run, 0 = the default */
    const char *emits[MAX_EMITS];
    int emit_count;
    BoardSimOptions board;
} Options;

static void usage(FILE *f) {
    fprintf(f,
        "JLang " JL_VERSION " - a small language for learning how hardware really works\n"
        "\n"
        "usage:\n"
        "  jlang new <name>              create a starter project in ./<name>\n"
        "  jlang run <file.jlang>        run a script on the simulated board\n"
        "  jlang check <file.jlang>      check for errors without running\n"
        "  jlang xray <file.jlang>       run, showing every instruction and the stack\n"
        "  jlang dis <file.jlang>        print the compiled bytecode\n"
        "  jlang repl                    interactive prompt\n"
        "  jlang emu <file.jlang>        compile to RISC-V and run it on the built-in emulator\n"
        "  jlang build <file.jlang>      compile to RISC-V assembly for a real board (-o out.s)\n"
        "  jlang asm <file.s>            assemble RISC-V assembly and run it on the emulator\n"
        "  jlang svd <chip.svd>          print a chip's registers as consts (--only GPIO,UART0.FIFO\n"
        "                                keeps some, -o file.jlang writes them to a file)\n"
        "  jlang wcet <file.jlang>       the most time the script and each handler can take\n"
        "                                (--mhz <n> sets the clock, default 160)\n"
        "  jlang version\n"
        "\n"
        "options for run / xray:\n"
        "  --emit <event>                after the script, fire an event (repeatable)\n"
        "  --pin <n>=<0|1>               value the simulated pin n reads as input\n"
        "  --analog <n>=<0..4095>        value analog(n) reads (default 2048)\n"
        "  --distance <cm>               what distance() measures (default 100)\n"
        "  --temperature <C>             what the I2C temperature sensor reads (default 24)\n"
        "  --answer <text>               what the simulated ask() replies\n"
        "  --heap <bytes>                heap size (default %u)\n"
        "  --max-cycles <n>              stop after n instructions (catches endless loops)\n"
        "  --realtime                    make delay() actually wait; in a terminal,\n"
        "                                LED panel frames animate in place, in colour\n"
        "  --panel <w>x<h>               LED panel size (default 60x7)\n"
        "  --vcd <file.vcd>              record every pin as a waveform for GTKWave\n"
        "  --for <seconds>               how long every/after timers run (default 10,\n"
        "                                or forever with --realtime)\n"
        "  --stats                       print instruction counts and heap use\n"
        "  --no-leak-check               don't report unfreed memory at exit\n"
        "\n"
        "options for emu / build:\n"
        "  --emit, --pin, --max-cycles,  as for run (emu only)\n"
        "  --vcd\n"
        "  --pipeline [rows]             show how a 5-stage pipeline runs the code: a diagram\n"
        "                                of the first rows instructions (default 16) and the\n"
        "                                stalls and flushes of the whole run (emu only)\n"
        "  --asm                         print the generated assembly instead of running\n"
        "  --listing                     print addresses and machine code for every instruction\n"
        "  -o <file.s>                   where build writes the assembly\n"
        "  --bin <file>                  write the raw memory image instead of running (emu)\n",
        JL_DEFAULT_HEAP);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 == cap) {
            cap *= 2;
            char *bigger = realloc(buf, cap);
            if (bigger == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = bigger;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static bool parse_args(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof *o);
    o->heap = JL_DEFAULT_HEAP;
    o->mhz = 160;
    board_sim_default_options(&o->board);
    if (argc < 2) return false;
    o->command = argv[1];

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        bool has_value = i + 1 < argc;
        if (strcmp(a, "--stats") == 0) {
            o->stats = true;
        } else if (strcmp(a, "--realtime") == 0) {
            o->board.realtime = true;
        } else if (strcmp(a, "--no-leak-check") == 0) {
            o->no_leak_check = true;
        } else if (strcmp(a, "--listing") == 0) {
            o->listing = true;
        } else if (strcmp(a, "--asm") == 0) {
            o->print_asm = true;
        } else if (strcmp(a, "--bin") == 0 && has_value) {
            o->bin_path = argv[++i];
        } else if (strcmp(a, "--for") == 0 && has_value) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 86400) {
                fprintf(stderr, "jlang: --for must be 1 to 86400 (seconds)\n");
                return false;
            }
            o->run_for_s = (int)v;
        } else if (strcmp(a, "--only") == 0 && has_value) {
            o->only = argv[++i];
        } else if (strcmp(a, "--mhz") == 0 && has_value) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 1 || v > 10000) {
                fprintf(stderr, "jlang: --mhz must be 1 to 10000\n");
                return false;
            }
            o->mhz = (uint32_t)v;
        } else if (strcmp(a, "--pipeline") == 0) {
            o->pipeline_on = true;
            o->pipeline_rows = 16;
            if (has_value && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                char *end;
                long v = strtol(argv[++i], &end, 10);
                if (*end != '\0' || v < 0 || v > 64) {
                    fprintf(stderr, "jlang: --pipeline takes a number of rows from 0 to 64\n");
                    return false;
                }
                o->pipeline_rows = (int)v;
            }
        } else if (strcmp(a, "--vcd") == 0 && has_value) {
            o->vcd_path = argv[++i];
        } else if (strcmp(a, "-o") == 0 && has_value) {
            o->out_path = argv[++i];
        } else if (strcmp(a, "--emit") == 0 && has_value) {
            if (o->emit_count == MAX_EMITS) {
                fprintf(stderr, "jlang: too many --emit options\n");
                return false;
            }
            o->emits[o->emit_count++] = argv[++i];
        } else if (strcmp(a, "--answer") == 0 && has_value) {
            o->board.ai_answer = argv[++i];
        } else if (strcmp(a, "--heap") == 0 && has_value) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 64 || v > 64L * 1024 * 1024) {
                fprintf(stderr, "jlang: --heap must be a number of bytes between 64 and 67108864\n");
                return false;
            }
            o->heap = (uint32_t)v;
        } else if (strcmp(a, "--max-cycles") == 0 && has_value) {
            char *end;
            unsigned long long v = strtoull(argv[++i], &end, 10);
            if (*end != '\0' || v == 0) {
                fprintf(stderr, "jlang: --max-cycles must be a positive number\n");
                return false;
            }
            o->max_cycles = v;
        } else if (strcmp(a, "--pin") == 0 && has_value) {
            int pin, val;
            char extra;
            if (sscanf(argv[++i], "%d=%d%c", &pin, &val, &extra) != 2 || pin < 0 ||
                pin >= SIM_PINS || (val != 0 && val != 1)) {
                fprintf(stderr, "jlang: --pin expects <pin>=<0|1>, like --pin 4=1\n");
                return false;
            }
            o->board.pin_inputs[pin] = val;
        } else if (strcmp(a, "--panel") == 0 && has_value) {
            int w, h;
            char extra;
            if (sscanf(argv[++i], "%dx%d%c", &w, &h, &extra) != 2 || w < 1 || h < 1 || w > PANEL_MAX_W ||
                h > PANEL_MAX_H) {
                fprintf(stderr, "jlang: --panel expects <width>x<height>, up to %dx%d, like --panel 32x8\n",
                        PANEL_MAX_W, PANEL_MAX_H);
                return false;
            }
            o->board.panel_w = w;
            o->board.panel_h = h;
        } else if (strcmp(a, "--analog") == 0 && has_value) {
            int pin, val;
            char extra;
            if (sscanf(argv[++i], "%d=%d%c", &pin, &val, &extra) != 2 || pin < 0 || pin >= SIM_PINS || val < 0 ||
                val > 4095) {
                fprintf(stderr, "jlang: --analog expects <pin>=<0..4095>, like --analog 1=3000\n");
                return false;
            }
            o->board.analog_inputs[pin] = val;
        } else if (strcmp(a, "--temperature") == 0 && has_value) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < -55 || v > 125) {
                fprintf(stderr, "jlang: --temperature must be -55 to 125 (degrees C)\n");
                return false;
            }
            o->board.temperature_c = (int)v;
        } else if (strcmp(a, "--distance") == 0 && has_value) {
            char *end;
            long v = strtol(argv[++i], &end, 10);
            if (*end != '\0' || v < 2 || v > 400) {
                fprintf(stderr, "jlang: --distance must be 2 to 400 (cm)\n");
                return false;
            }
            o->board.distance_cm = (int)v;
        } else if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "jlang: unknown or incomplete option '%s'\n", a);
            return false;
        } else if (o->file == NULL) {
            o->file = a;
        } else {
            fprintf(stderr, "jlang: unexpected argument '%s'\n", a);
            return false;
        }
    }
    return true;
}

static JlVM *make_vm(const Options *o) {
    JlConfig cfg;
    jl_default_config(&cfg);
    cfg.heap_size = o->heap;
    cfg.max_cycles = o->max_cycles;
    JlVM *vm = jl_new(&cfg);
    if (vm == NULL) {
        fprintf(stderr, "jlang: could not create the VM (out of memory?)\n");
        return NULL;
    }
    BoardSimOptions board = o->board;
    board.ansi_frames = board.realtime && jl_isatty_stdout();
    board_sim_register(vm, &board);
    return vm;
}

static bool save_vcd(const char *path, Vcd *vcd);

static int run_file(const Options *opts, bool xray) {
    char *src = read_file(opts->file);
    if (src == NULL) {
        fprintf(stderr, "jlang: can't read '%s': %s\n", opts->file, strerror(errno));
        return EXIT_NOFILE;
    }
    Options copy = *opts;
    const Options *o = &copy;
    Vcd *vcd = opts->vcd_path ? vcd_new() : NULL;
    copy.board.vcd = vcd;
    JlVM *vm = make_vm(o);
    if (vm == NULL) {
        free(src);
        return EXIT_RUNTIME;
    }
    jl_set_trace(vm, xray);

    int code = 0;
    JlResult r = jl_run_source(vm, src);
    if (r == JL_COMPILE_ERROR) code = EXIT_COMPILE;
    if (r == JL_RUNTIME_ERROR) code = EXIT_RUNTIME;

    for (int i = 0; code == 0 && i < o->emit_count; i++) {
        r = jl_emit(vm, o->emits[i]);
        if (r == JL_NO_HANDLER) {
            fflush(stdout);
            fprintf(stderr, "jlang: --emit %s: the script has no 'on %s { ... }' handler\n",
                    o->emits[i], o->emits[i]);
            code = EXIT_RUNTIME;
        } else if (r != JL_OK) {
            code = EXIT_RUNTIME;
        }
    }

    if (code == 0) {
        /* Then the timers, as a board's main loop would run them. */
        uint32_t until = o->run_for_s > 0 ? (uint32_t)o->run_for_s * 1000u : o->board.realtime ? 0 : 10000u;
        if (board_sim_run_timers(vm, until) != JL_OK) code = EXIT_RUNTIME;
    }

    if (code != EXIT_COMPILE) {
        if (o->stats) jl_print_stats(vm);
        /* After a crash, unfreed memory is expected; only report leaks
         * for programs that finished normally. */
        if (!o->no_leak_check && code == 0) jl_report_leaks(vm);
    }
    fflush(stdout);
    jl_free(vm);
    free(src);
    if (code != EXIT_COMPILE && !save_vcd(o->vcd_path, vcd) && code == 0) code = EXIT_USAGE;
    vcd_free(vcd);
    return code;
}

/* Writes the --vcd file, if one was asked for. Returns false on failure. */
static bool save_vcd(const char *path, Vcd *vcd) {
    if (path == NULL || vcd == NULL) return true;
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "jlang: can't write '%s': %s\n", path, strerror(errno));
        return false;
    }
    bool ok = vcd_write(vcd, f);
    if (fclose(f) != 0) ok = false;
    if (!ok) fprintf(stderr, "jlang: couldn't write all of '%s'\n", path);
    return ok;
}

/* ---- jlang emu / jlang build (RISC-V) -------------------------------- */

static void write_stdout(void *ud, const char *d, size_t n) {
    (void)ud;
    fwrite(d, 1, n, stdout);
}

static void write_stderr(void *ud, const char *d, size_t n) {
    (void)ud;
    fflush(stdout);
    fwrite(d, 1, n, stderr);
}

static int native(const Options *o, bool emulate) {
    char *src = read_file(o->file);
    if (src == NULL) {
        fprintf(stderr, "jlang: can't read '%s': %s\n", o->file, strerror(errno));
        return EXIT_NOFILE;
    }
    JlVM *vm = make_vm(o);
    if (vm == NULL) {
        free(src);
        return EXIT_RUNTIME;
    }
    RvGenOptions gen = {emulate, (const char **)o->emits, o->emit_count};
    char *assembly = rv_generate(vm, src, &gen);
    jl_free(vm);
    free(src);
    if (assembly == NULL) return EXIT_COMPILE;

    if (o->print_asm) {
        fputs(assembly, stdout);
        free(assembly);
        return 0;
    }

    RvImage img;
    if (!rv_assemble(assembly, &img, o->listing, !emulate)) {
        fprintf(stderr, "%s\n", img.error);
        free(assembly);
        return EXIT_COMPILE;
    }
    if (o->listing && img.listing) fputs(img.listing, stdout);
    if (o->wcet) {
        rv_wcet_report(&img, o->mhz, write_stdout, NULL);
        rv_image_free(&img);
        free(assembly);
        return 0;
    }

    int code = 0;
    if (o->bin_path) {
        /* raw memory image from address 0 (code, constants, data) */
        FILE *f = fopen(o->bin_path, "wb");
        if (f == NULL) {
            fprintf(stderr, "jlang: can't write '%s': %s\n", o->bin_path, strerror(errno));
            code = EXIT_USAGE;
        } else {
            uint32_t end = img.size;
            uint32_t bss;
            if (rv_symbol(&img, "jl_vstack", &bss)) end = bss;
            fwrite(img.image, 1, end, f);
            fclose(f);
        }
    } else if (emulate) {
        if (!o->listing) {
            RvRunOptions run;
            memset(&run, 0, sizeof run);
            for (int i = 0; i < 32; i++) run.pin_inputs[i] = o->board.pin_inputs[i];
            run.max_cycles = o->max_cycles;
            run.out = write_stdout;
            run.err = write_stderr;
            run.panel_w = o->board.panel_w;
            run.panel_h = o->board.panel_h;
            run.ansi_frames = o->board.realtime && jl_isatty_stdout();
            run.vcd = o->vcd_path ? vcd_new() : NULL;
            RvPipe *pipe = NULL;
            if (o->pipeline_on) {
                pipe = rv_pipe_new(&img, o->pipeline_rows);
                run.trace = rv_pipe_step;
                run.trace_ud = pipe;
            }
            uint64_t cycles = 0;
            code = rv_run(&img, &run, &cycles);
            fflush(stdout);
            if (pipe) {
                rv_pipe_report(pipe, &img, write_stdout, NULL);
                rv_pipe_free(pipe);
            }
            if (!save_vcd(o->vcd_path, run.vcd) && code == 0) code = EXIT_USAGE;
            vcd_free(run.vcd);
            if (o->stats) {
                fprintf(stderr, "--- stats ---\nRISC-V instructions executed: %llu\ncode size: %u bytes\n",
                        (unsigned long long)cycles, (unsigned)img.text_size);
            }
        }
    } else {
        char path[1024];
        if (o->out_path) {
            snprintf(path, sizeof path, "%s", o->out_path);
        } else {
            snprintf(path, sizeof path, "%s", o->file);
            char *dot = strrchr(path, '.');
            if (dot && !strchr(dot, '/')) *dot = '\0';
            strncat(path, ".s", sizeof path - strlen(path) - 1);
        }
        FILE *f = fopen(path, "w");
        if (f == NULL) {
            fprintf(stderr, "jlang: can't write '%s': %s\n", path, strerror(errno));
            code = EXIT_USAGE;
        } else {
            fputs(assembly, f);
            fclose(f);
            if (!o->listing) {
                printf("wrote %s (%u bytes of RV32IM code)\n", path, (unsigned)img.text_size);
            }
        }
    }
    rv_image_free(&img);
    free(assembly);
    return code;
}

/* jlang svd chip.svd [--only GPIO,LEDC] [-o regs.jlang] */
static int svd_command(const Options *o) {
    char *xml = read_file(o->file);
    if (xml == NULL) {
        fprintf(stderr, "jlang: can't read '%s': %s\n", o->file, strerror(errno));
        return EXIT_NOFILE;
    }
    char *list[64];
    int n = 0;
    char *only = NULL;
    if (o->only) { /* strdup isn't in C99 */
        only = malloc(strlen(o->only) + 1);
        if (only) strcpy(only, o->only);
    }
    for (char *tok = only ? strtok(only, ",") : NULL; tok && n < 64; tok = strtok(NULL, ",")) list[n++] = tok;
    FILE *out = o->out_path ? fopen(o->out_path, "w") : stdout;
    if (out == NULL) {
        fprintf(stderr, "jlang: can't write '%s': %s\n", o->out_path, strerror(errno));
        free(xml);
        free(only);
        return EXIT_USAGE;
    }
    const char *base = strrchr(o->file, '/');
    char err[256];
    bool ok = svd_to_jlang(xml, base ? base + 1 : o->file, list, n, out, err, sizeof err);
    if (out != stdout) fclose(out);
    if (!ok) fprintf(stderr, "jlang: %s\n", err);
    free(xml);
    free(only);
    return ok ? 0 : EXIT_USAGE;
}

/* jlang asm file.s: assemble RISC-V assembly and run it on the emulator */
static int run_assembly(const Options *o) {
    char *src = read_file(o->file);
    if (src == NULL) {
        fprintf(stderr, "jlang: can't read '%s': %s\n", o->file, strerror(errno));
        return EXIT_NOFILE;
    }
    RvImage img;
    bool ok = rv_assemble(src, &img, o->listing, false);
    free(src);
    if (!ok) {
        fprintf(stderr, "%s\n", img.error);
        return EXIT_COMPILE;
    }
    int code = 0;
    if (o->listing) {
        if (img.listing) fputs(img.listing, stdout);
    } else {
        RvRunOptions run;
        memset(&run, 0, sizeof run);
        for (int i = 0; i < 32; i++) run.pin_inputs[i] = o->board.pin_inputs[i];
        run.max_cycles = o->max_cycles;
        run.out = write_stdout;
        run.err = write_stderr;
        run.panel_w = o->board.panel_w;
        run.panel_h = o->board.panel_h;
        uint64_t cycles = 0;
        code = rv_run(&img, &run, &cycles);
        fflush(stdout);
        if (o->stats) fprintf(stderr, "--- stats ---\nRISC-V instructions executed: %llu\n", (unsigned long long)cycles);
    }
    rv_image_free(&img);
    return code;
}

static int compile_only(const Options *o, bool disassemble) {
    char *src = read_file(o->file);
    if (src == NULL) {
        fprintf(stderr, "jlang: can't read '%s': %s\n", o->file, strerror(errno));
        return EXIT_NOFILE;
    }
    JlVM *vm = make_vm(o);
    if (vm == NULL) {
        free(src);
        return EXIT_RUNTIME;
    }
    JlResult r = jl_disassemble_or_check(vm, src, disassemble);
    if (r == JL_OK && !disassemble) printf("ok: %s has no errors\n", o->file);
    fflush(stdout);
    jl_free(vm);
    free(src);
    return r == JL_OK ? 0 : EXIT_COMPILE;
}

/* ---- jlang repl ---------------------------------------------------- */

static int brace_balance(const char *s) {
    int depth = 0;
    bool in_str = false;
    for (; *s; s++) {
        if (in_str) {
            if (*s == '\\' && s[1]) s++;
            else if (*s == '"') in_str = false;
        } else if (*s == '"') {
            in_str = true;
        } else if (*s == '/' && s[1] == '/') {
            while (*s && *s != '\n') s++;
            if (!*s) break;
        } else if (*s == '{') {
            depth++;
        } else if (*s == '}') {
            depth--;
        }
    }
    return depth;
}

static int repl(const Options *o) {
    JlVM *vm = make_vm(o);
    if (vm == NULL) return EXIT_RUNTIME;
    printf("JLang %s. Type code and press Enter. Blocks { } can span lines.\n"
           "Try: print(0b1010 | 0x0F)    Quit: Ctrl-D\n", JL_VERSION);

    char *buf = NULL;
    size_t len = 0;
    char line[1024];
    for (;;) {
        fputs(len == 0 ? "jl> " : "... ", stdout);
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) break;
        size_t n = strlen(line);
        char *bigger = realloc(buf, len + n + 1);
        if (bigger == NULL) break;
        buf = bigger;
        memcpy(buf + len, line, n + 1);
        len += n;
        if (brace_balance(buf) > 0) continue;
        jl_run_source(vm, buf);
        len = 0;
        buf[0] = '\0';
    }
    putchar('\n');
    free(buf);
    jl_free(vm);
    return 0;
}

/* ---- jlang new ----------------------------------------------------- */

static const char *STARTER_MAIN =
    "// main.jlang - your first JLang program.\n"
    "// Run it:   jlang run main.jlang --emit button\n"
    "\n"
    "let LED = 2      // the LED is wired to pin 2\n"
    "let BUTTON = 4   // the button is wired to pin 4\n"
    "\n"
    "pin_mode(LED, \"out\")\n"
    "pin_mode(BUTTON, \"in_pullup\")\n"
    "\n"
    "// Blink 3 times. delay() uses virtual time in the simulator,\n"
    "// so this finishes instantly but millis() still counts the time.\n"
    "for i in 0..3 {\n"
    "    pin_write(LED, 1)\n"
    "    delay(250)\n"
    "    pin_write(LED, 0)\n"
    "    delay(250)\n"
    "}\n"
    "printf(\"blinked 3 times in {} ms\\n\", millis())\n"
    "\n"
    "// A status register: bits 7..4 = mode, bit 0 = LED on.\n"
    "let status = 0\n"
    "status[7..4] = 0b1010\n"
    "status[0] = 1\n"
    "printf(\"status = {b8} ({x2})\\n\", status, status)\n"
    "\n"
    "// Runs every time the 'button' event fires (like an interrupt).\n"
    "on button {\n"
    "    print(\"button pressed!\")\n"
    "    pin_write(LED, 1)\n"
    "}\n";

static const char *STARTER_README =
    "# %s\n"
    "\n"
    "A [JLang](https://github.com/jafarnz/JLang) project.\n"
    "\n"
    "```bash\n"
    "jlang run main.jlang --emit button   # run on the simulated board\n"
    "jlang check main.jlang               # check for errors\n"
    "jlang xray main.jlang                # watch the virtual machine work\n"
    "```\n";

static const char *STARTER_AGENTS =
    "# Notes for AI coding agents\n"
    "\n"
    "This project is written in JLang, a small language for microcontrollers.\n"
    "\n"
    "- Check code with `jlang check main.jlang` (exit 0 = ok, 65 = compile error).\n"
    "- Run with `jlang run main.jlang`; add `--emit <event>` to fire `on <event>` handlers.\n"
    "- Full reference: https://github.com/jafarnz/JLang/blob/main/docs/llms.txt\n"
    "\n"
    "Key rules:\n"
    "- Ints are 32-bit and wrap. No floats. Strings are read-only.\n"
    "- No semicolons. Blocks use { }. Conditions need no parentheses.\n"
    "- `let x = 1`, `fn add(a, b) { return a + b }`, `for i in 0..10 { }`.\n"
    "- `x[3]` is bit 3 of an int; `x[7..4] = 5` sets bits 7..4.\n"
    "- `alloc(n)` returns a buf of n bytes that MUST be freed with `free(buf)`.\n"
    "- Strings returned by `ask()` live on the heap and must be freed too.\n"
    "- Assignment is a statement, never inside an expression.\n";

static bool write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wx"); /* x: never overwrite */
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static int new_project(const char *name) {
    if (name == NULL) {
        fprintf(stderr, "usage: jlang new <name>\n");
        return EXIT_USAGE;
    }
    if (jl_mkdir(name) != 0) {
        fprintf(stderr, "jlang: can't create folder '%s': %s\n", name, strerror(errno));
        return EXIT_USAGE;
    }
    char path[1024], readme[1024];
    snprintf(readme, sizeof readme, STARTER_README, name);
    snprintf(path, sizeof path, "%s/main.jlang", name);
    bool ok = write_text(path, STARTER_MAIN);
    snprintf(path, sizeof path, "%s/README.md", name);
    ok = ok && write_text(path, readme);
    snprintf(path, sizeof path, "%s/AGENTS.md", name);
    ok = ok && write_text(path, STARTER_AGENTS);
    if (!ok) {
        fprintf(stderr, "jlang: couldn't write the project files\n");
        return EXIT_USAGE;
    }
    printf("created %s/\n  main.jlang     your program\n  README.md\n  AGENTS.md   "
           "instructions for AI coding agents\n\nnext:\n  cd %s\n  jlang run main.jlang "
           "--emit button\n", name, name);
    return 0;
}

int main(int argc, char **argv) {
    Options o;
    if (!parse_args(argc, argv, &o)) {
        if (argc < 2) usage(stderr);
        else fprintf(stderr, "run 'jlang help' for usage\n");
        return EXIT_USAGE;
    }
    const char *cmd = o.command;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("jlang %s\n", JL_VERSION);
        return 0;
    }
    if (strcmp(cmd, "new") == 0) return new_project(o.file);
    if (strcmp(cmd, "repl") == 0) return repl(&o);

    bool known = strcmp(cmd, "run") == 0 || strcmp(cmd, "xray") == 0 ||
                 strcmp(cmd, "check") == 0 || strcmp(cmd, "dis") == 0 ||
                 strcmp(cmd, "emu") == 0 || strcmp(cmd, "build") == 0 || strcmp(cmd, "asm") == 0 ||
                 strcmp(cmd, "wcet") == 0 || strcmp(cmd, "svd") == 0;
    if (!known) {
        fprintf(stderr, "jlang: unknown command '%s'\n\n", cmd);
        usage(stderr);
        return EXIT_USAGE;
    }
    if (o.file == NULL) {
        fprintf(stderr, "usage: jlang %s <file.jlang>\n", cmd);
        return EXIT_USAGE;
    }
    if (strcmp(cmd, "run") == 0) return run_file(&o, false);
    if (strcmp(cmd, "emu") == 0) return native(&o, true);
    if (strcmp(cmd, "svd") == 0) return svd_command(&o);
    if (strcmp(cmd, "wcet") == 0) {
        o.wcet = true;
        return native(&o, true);
    }
    if (strcmp(cmd, "asm") == 0) return run_assembly(&o);
    if (strcmp(cmd, "build") == 0) return native(&o, false);
    if (strcmp(cmd, "xray") == 0) return run_file(&o, true);
    if (strcmp(cmd, "check") == 0) return compile_only(&o, false);
    return compile_only(&o, true);
}
