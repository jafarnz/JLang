/*
 * compiler.c - turns JLang source code into bytecode in ONE pass.
 *
 * There is no syntax tree: as soon as the parser recognises a piece of
 * code it emits the bytecode for it. This keeps memory use tiny, which
 * matters when JLang compiles scripts on a microcontroller.
 *
 * Expressions use a "Pratt parser": every token type has a rule saying
 * how to parse it at the start of an expression (prefix) and in the
 * middle of one (infix), plus its precedence. See the rules[] table.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "lexer.h"

/* Lowest to highest. Note: unlike C, the bitwise operators bind
 * tighter than comparisons, so `x & 1 == 0` means `(x & 1) == 0`. */
typedef enum {
    PREC_NONE,
    PREC_ASSIGN,     /* =  +=  ... (statements only) */
    PREC_OR,         /* || */
    PREC_AND,        /* && */
    PREC_EQUALITY,   /* == != */
    PREC_COMPARISON, /* < <= > >= */
    PREC_BITOR,      /* | */
    PREC_BITXOR,     /* ^ */
    PREC_BITAND,     /* & */
    PREC_SHIFT,      /* << >> */
    PREC_TERM,       /* + - */
    PREC_FACTOR,     /* * / % */
    PREC_UNARY,      /* - ! ~ */
    PREC_CALL,       /* f()  x[i] */
    PREC_PRIMARY
} Precedence;

typedef struct {
    Token name;
    int depth; /* -1 while its initializer is being compiled */
} Local;

#define MAX_LOOP_JUMPS 64

typedef struct Loop {
    struct Loop *enclosing;
    bool is_for;
    int start; /* bytecode offset of the loop condition */
    int depth; /* scope depth outside the loop body */
    int breaks[MAX_LOOP_JUMPS];
    int break_count;
    int continues[MAX_LOOP_JUMPS];
    int continue_count;
} Loop;

#define MAX_DEFERS 64
#define MAX_USE_DEPTH 16
#define MAX_USED_FILES 128

typedef struct {
    Lexer lx;
    Token cur;
    Token prev;
    const char *end;
    const char *src;
    const char *file_path;
    int depth;
} DeferEntry;

/* State for the function currently being compiled. */
typedef struct FnState {
    struct FnState *enclosing;
    int fn;
    bool is_script;
    Local locals[256];
    int local_count;
    int scope_depth;
    Loop *loop;
    DeferEntry defers[MAX_DEFERS];
    int defer_count;
} FnState;

typedef struct {
    JlVM *vm;
    const char *src;
    const char *file_path; /* NULL for root file, normalized path inside 'use' */
    const char *base_path; /* normalized path of current file for relative 'use' */
    bool in_use;
    Lexer lx;
    Token cur;
    Token prev;
    bool had_error;
    bool panic;
    bool in_defer;
    int nesting; /* expressions and blocks inside each other */
    int errors;
    FnState *fs;

    /* Header-free multi-file `use "path.jlang"` state. */
    const char *use_stack[MAX_USE_DEPTH];
    int use_depth;
    char *used_files[MAX_USED_FILES];
    int used_count;
    char *owned_srcs[MAX_USED_FILES];
    int owned_src_count;

    /* The most recent plain variable read. Lets `reg[3] = 1` write the
     * changed int back into the variable `reg`. */
    int var_end;
    uint8_t var_set_op;
    uint8_t var_arg;

    /* Where the most recent + ended, so a + chained onto it knows its left
     * value is a temporary string that can be freed after joining. */
    int add_end;
    Chunk *add_chunk;

    /* Most recent plain Enum.MEMBER expression, for exhaustive enum match checking. */
    int last_enum_end;
    uint16_t last_enum_si;
    uint16_t last_enum_mem_si;
} Compiler;

typedef void (*ParseFn)(Compiler *c, bool can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence prec;
} ParseRule;

static void expression(Compiler *c);
static void parse_precedence(Compiler *c, Precedence prec);
static void parse_precedence_inner(Compiler *c, Precedence prec);
static const ParseRule *get_rule(TokenType type);
static void declaration(Compiler *c);
static void statement(Compiler *c);
static void block(Compiler *c);

/* ------------------------------------------------------------------ */
/* Error reporting                                                     */
/* ------------------------------------------------------------------ */

static const char *line_start_in(const char *src, int line) {
    const char *p = src;
    for (int l = 1; *p && l < line; p++) {
        if (*p == '\n') l++;
    }
    return p;
}

static const char *line_start_of(const Compiler *c, int line) {
    return line_start_in(c->src, line);
}

/* Prints the source line and a ^ under the problem. */
static void show_source_in(JlVM *vm, const char *src, int line, int col) {
    const char *p = line_start_in(src, line);
    const char *e = p;
    while (*e && *e != '\n') e++;
    vm_errf(vm, "  %4d | %.*s\n", line, (int)(e - p), p);
    vm_errf(vm, "       | ");
    for (int i = 0; i < col - 1 && p + i < e; i++) {
        vm_write(vm, true, p[i] == '\t' ? "\t" : " ", 1);
    }
    vm_errf(vm, "^\n");
}

static void show_source(Compiler *c, int line, int col) {
    show_source_in(c->vm, c->src, line, col);
}

static void error_at(Compiler *c, const Token *t, const char *msg) {
    if (c->panic) return;
    c->panic = true;
    c->had_error = true;
    if (++c->errors > 10) return;
    if (t->type == TOK_EOF) {
        if (c->file_path) {
            vm_errf(c->vm, "error (%s, line %d): %s (reached the end of the file)\n",
                    c->file_path, t->line, msg);
        } else {
            vm_errf(c->vm, "error (line %d): %s (reached the end of the file)\n",
                    t->line, msg);
        }
    } else if (c->file_path) {
        vm_errf(c->vm, "error (%s, line %d, col %d): %s\n",
                c->file_path, t->line, t->col, msg);
    } else {
        vm_errf(c->vm, "error (line %d, col %d): %s\n", t->line, t->col, msg);
    }
    show_source(c, t->line, t->col);
}

static void error(Compiler *c, const char *msg) { error_at(c, &c->prev, msg); }
static void error_at_current(Compiler *c, const char *msg) {
    error_at(c, &c->cur, msg);
}

static void errorf(Compiler *c, const char *fmt, ...) JL_PRINTF(2, 3);
static void errorf(Compiler *c, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    error(c, buf);
}

/* ------------------------------------------------------------------ */
/* Token handling                                                      */
/* ------------------------------------------------------------------ */

static void advance(Compiler *c) {
    c->prev = c->cur;
    for (;;) {
        c->cur = lexer_next(&c->lx);
        if (c->cur.type != TOK_ERROR) break;
        char msg[160];
        snprintf(msg, sizeof msg, "%.*s", c->cur.length, c->cur.start);
        if (strcmp(msg, "unexpected character") == 0) {
            const char *p = line_start_of(c, c->cur.line);
            snprintf(msg, sizeof msg, "unexpected character '%c'",
                     p[c->cur.col - 1]);
        }
        error_at(c, &c->cur, msg);
    }
}

static bool check(const Compiler *c, TokenType t) { return c->cur.type == t; }

static bool match(Compiler *c, TokenType t) {
    if (!check(c, t)) return false;
    advance(c);
    return true;
}

static void consume(Compiler *c, TokenType t, const char *msg) {
    if (check(c, t)) {
        advance(c);
        return;
    }
    error_at_current(c, msg);
}

/* ------------------------------------------------------------------ */
/* Emitting bytecode                                                   */
/* ------------------------------------------------------------------ */

static Chunk *chunk(Compiler *c) { return &c->vm->prog.fns[c->fs->fn].chunk; }

static void emit(Compiler *c, uint8_t byte) {
    chunk_write(chunk(c), byte, c->prev.line);
}

static void emit2(Compiler *c, uint8_t a, uint8_t b) {
    emit(c, a);
    emit(c, b);
}

static void emit_u16(Compiler *c, uint16_t v) {
    emit(c, (uint8_t)(v >> 8));
    emit(c, (uint8_t)(v & 0xFF));
}

static void emit_const(Compiler *c, Value v) {
    int i = chunk_add_const(chunk(c), v);
    if (i < 0) {
        error(c, "too many constants in one function (max 65536)");
        return;
    }
    emit(c, OP_CONST);
    emit_u16(c, (uint16_t)i);
}

static int emit_jump(Compiler *c, uint8_t op) {
    emit(c, op);
    emit(c, 0xFF);
    emit(c, 0xFF);
    return chunk(c)->count - 2;
}

static void patch_jump(Compiler *c, int at) {
    int jump = chunk(c)->count - at - 2;
    if (jump > 0xFFFF) {
        error(c, "this block of code is too big to jump over");
        return;
    }
    chunk(c)->code[at] = (uint8_t)((jump >> 8) & 0xFF);
    chunk(c)->code[at + 1] = (uint8_t)(jump & 0xFF);
}

static void emit_loop(Compiler *c, int start) {
    emit(c, OP_LOOP);
    int off = chunk(c)->count - start + 2;
    if (off > 0xFFFF) error(c, "this loop body is too big");
    emit_u16(c, (uint16_t)off);
}

/* ------------------------------------------------------------------ */
/* Scopes and variables                                                */
/* ------------------------------------------------------------------ */

static void begin_scope(Compiler *c) { c->fs->scope_depth++; }

static void emit_defers(Compiler *c, int min_depth, bool pop) {
    FnState *fs = c->fs;
    int i = fs->defer_count;
    while (i > 0 && fs->defers[i - 1].depth >= min_depth) {
        DeferEntry d = fs->defers[--i];
        Lexer saved_lx = c->lx;
        Token saved_cur = c->cur;
        Token saved_prev = c->prev;
        const char *saved_src = c->src;
        const char *saved_file = c->file_path;
        bool saved_in = c->in_defer;
        int saved_var_end = c->var_end;
        Chunk *saved_add = c->add_chunk;

        c->lx = d.lx;
        c->lx.limit = d.end;
        c->cur = d.cur;
        c->prev = d.prev;
        c->src = d.src;
        c->file_path = d.file_path;
        c->in_defer = true;
        c->var_end = -1;
        c->add_chunk = NULL;
        statement(c);

        c->lx = saved_lx;
        c->cur = saved_cur;
        c->prev = saved_prev;
        c->src = saved_src;
        c->file_path = saved_file;
        c->in_defer = saved_in;
        c->var_end = saved_var_end;
        c->add_chunk = saved_add;
    }
    if (pop) fs->defer_count = i;
}

static void end_scope(Compiler *c) {
    FnState *fs = c->fs;
    emit_defers(c, fs->scope_depth, true);
    fs->scope_depth--;
    while (fs->local_count > 0 &&
           fs->locals[fs->local_count - 1].depth > fs->scope_depth) {
        emit(c, OP_POP);
        fs->local_count--;
        /* debug info: the variable in this slot ends here */
        Chunk *ch = chunk(c);
        for (int i = ch->var_count - 1; i >= 0; i--)
            if (ch->vars[i].slot == fs->local_count && ch->vars[i].to < 0) {
                ch->vars[i].to = ch->count;
                break;
            }
    }
}

/* For break/continue: pop the loop body's locals without forgetting them. */
static void pop_locals_deeper_than(Compiler *c, int depth) {
    FnState *fs = c->fs;
    for (int i = fs->local_count - 1; i >= 0 && fs->locals[i].depth > depth; i--) {
        emit(c, OP_POP);
    }
}

static bool same_name(const Token *a, const Token *b) {
    return a->length == b->length &&
           memcmp(a->start, b->start, (size_t)a->length) == 0;
}

static void declare_local(Compiler *c, Token name) {
    FnState *fs = c->fs;
    for (int i = fs->local_count - 1; i >= 0; i--) {
        Local *l = &fs->locals[i];
        if (l->depth != -1 && l->depth < fs->scope_depth) break;
        if (same_name(&name, &l->name)) {
            errorf(c, "'%.*s' is already declared in this block",
                   name.length, name.start);
            return;
        }
    }
    if (fs->local_count == 256) {
        error(c, "too many local variables in one function (max 256)");
        return;
    }
    fs->locals[fs->local_count].name = name;
    fs->locals[fs->local_count].depth = -1;
    fs->local_count++;
}

static void mark_initialized(Compiler *c) {
    int slot = c->fs->local_count - 1;
    Local *l = &c->fs->locals[slot];
    l->depth = c->fs->scope_depth;
    /* Debug info: from here on, this slot holds this variable. Hidden
       locals such as a for loop's "(end)" are left out. */
    if (l->name.length > 0 && l->name.start[0] != '(') {
        Chunk *ch = chunk(c);
        int si = program_intern(&c->vm->prog, l->name.start, (uint32_t)l->name.length);
        if (si >= 0) {
            if (ch->var_count == ch->var_cap) {
                ch->var_cap = ch->var_cap ? ch->var_cap * 2 : 8;
                ch->vars = jl_xrealloc(ch->vars, (size_t)ch->var_cap * sizeof *ch->vars);
            }
            ch->vars[ch->var_count].name = (uint16_t)si;
            ch->vars[ch->var_count].slot = (uint8_t)slot;
            ch->vars[ch->var_count].from = ch->count;
            ch->vars[ch->var_count++].to = -1;
        }
    }
}

static int resolve_local(Compiler *c, const Token *name) {
    FnState *fs = c->fs;
    for (int i = fs->local_count - 1; i >= 0; i--) {
        if (same_name(name, &fs->locals[i].name)) {
            if (fs->locals[i].depth == -1) {
                errorf(c, "can't use '%.*s' inside its own initializer",
                       name->length, name->start);
            }
            return i;
        }
    }
    return -1;
}

static int global_slot(Compiler *c, Token name) {
    int i = vm_global_find(c->vm, name.start, name.length);
    if (i < 0) i = vm_global_add(c->vm, name.start, name.length);
    if (i < 0) {
        error(c, "too many global names (max 256)");
        return 0;
    }
    return i;
}

static int global_use(Compiler *c, Token name) {
    int i = global_slot(c, name);
    Global *g = &c->vm->globals[i];
    if (!g->declared && g->use_line == 0) {
        g->use_line = name.line;
        g->use_col = name.col;
        g->use_src = c->src;
        g->use_file = c->file_path;
    }
    return i;
}

static int global_declare(Compiler *c, Token name) {
    int i = global_slot(c, name);
    Global *g = &c->vm->globals[i];
    if (g->declared) {
        if (g->value.type == VAL_NATIVE) {
            errorf(c, "'%.*s' is a built-in function and can't be redeclared",
                   name.length, name.start);
        } else {
            errorf(c, "'%.*s' is already declared", name.length, name.start);
        }
    }
    g->declared = true;
    return i;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                         */
/* ------------------------------------------------------------------ */

static bool is_compound(TokenType t) { return t >= TOK_PLUS_EQ && t <= TOK_SHR_EQ; }

static uint8_t compound_op(TokenType t) {
    switch (t) {
    case TOK_PLUS_EQ: return OP_ADD;
    case TOK_MINUS_EQ: return OP_SUB;
    case TOK_STAR_EQ: return OP_MUL;
    case TOK_SLASH_EQ: return OP_DIV;
    case TOK_PERCENT_EQ: return OP_MOD;
    case TOK_AMP_EQ: return OP_BAND;
    case TOK_PIPE_EQ: return OP_BOR;
    case TOK_CARET_EQ: return OP_BXOR;
    case TOK_SHL_EQ: return OP_SHL;
    default: return OP_SHR;
    }
}

static void expression(Compiler *c) { parse_precedence(c, PREC_OR); }

static void parse_precedence(Compiler *c, Precedence prec) {
    if (++c->nesting > JL_NESTING_MAX) {
        error_at_current(c, "this is nested too deeply (more than " JL_STR(JL_NESTING_MAX) " levels)");
        while (!check(c, TOK_EOF)) advance(c); /* skip the rest rather than report each level */
        c->nesting--;
        return;
    }
    parse_precedence_inner(c, prec);
    c->nesting--;
}

static void parse_precedence_inner(Compiler *c, Precedence prec) {
    advance(c);
    ParseFn prefix = get_rule(c->prev.type)->prefix;
    if (prefix == NULL) {
        error(c, "expected an expression here");
        return;
    }
    bool can_assign = prec <= PREC_ASSIGN;
    prefix(c, can_assign);

    while (prec <= get_rule(c->cur.type)->prec) {
        advance(c);
        get_rule(c->prev.type)->infix(c, can_assign);
    }

    if (check(c, TOK_EQ) || is_compound(c->cur.type)) {
        if (can_assign) {
            error_at_current(c, "can't assign to this (you can assign to a "
                                "variable like x = 1 or an element like buf[0] = 1)");
        } else {
            error_at_current(c, "assignment must be a statement of its own, "
                                "not part of an expression (did you mean == ?)");
        }
        /* Skip the '=' and the value so the rest of the line still parses
         * and we don't report confusing follow-up errors. */
        advance(c);
        expression(c);
        c->panic = false; /* recovered: keep parsing normally */
    }
}

static void grouping(Compiler *c, bool can_assign) {
    (void)can_assign;
    expression(c);
    consume(c, TOK_RPAREN, "expected ')' to close the '('");
}

static void number(Compiler *c, bool can_assign) {
    (void)can_assign;
    const char *s = c->prev.start;
    int n = c->prev.length;
    int base = 10, i = 0;
    /* Times: 500ms is 500 and 2s is 2000, both in milliseconds. */
    uint64_t scale = 1;
    if (n > 2 && s[n - 2] == 'm' && s[n - 1] == 's') {
        n -= 2;
    } else if (n > 1 && s[n - 1] == 's') {
        n -= 1;
        scale = 1000;
    }
    if (n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (n > 1 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        i = 2;
    }
    uint64_t v = 0;
    int digits = 0;
    for (; i < n; i++) {
        if (s[i] == '_') continue;
        int d = isdigit((unsigned char)s[i]) ? s[i] - '0'
                                             : tolower((unsigned char)s[i]) - 'a' + 10;
        v = v * (uint64_t)base + (uint64_t)d;
        digits++;
        if (v * scale > 0xFFFFFFFFu) {
            error(c, "number is too big for 32 bits (the max is 4294967295, or 0xFFFFFFFF)");
            return;
        }
    }
    if (digits == 0) {
        error(c, "this number literal has no digits");
        return;
    }
    /* Literals above 2147483647 wrap to negative, exactly like storing a
     * 32-bit pattern in a signed register. Handy for masks: 0xFFFFFFFF. */
    emit_const(c, int_val((int32_t)(uint32_t)(v * scale)));
}

/* Decodes \n \t \r \0 \\ \" \' \xHH. Returns the new length or -1. */
static int decode_escapes(Compiler *c, const char *s, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] != '\\') {
            out[o++] = s[i];
            continue;
        }
        i++;
        switch (s[i]) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case '0': out[o++] = '\0'; break;
        case '\\': out[o++] = '\\'; break;
        case '"': out[o++] = '"'; break;
        case '\'': out[o++] = '\''; break;
        case 'x':
            if (i + 2 < n && isxdigit((unsigned char)s[i + 1]) &&
                isxdigit((unsigned char)s[i + 2])) {
                char hex[3] = {s[i + 1], s[i + 2], '\0'};
                out[o++] = (char)strtol(hex, NULL, 16);
                i += 2;
                break;
            }
            error(c, "\\x must be followed by two hex digits, like \\x41");
            return -1;
        default:
            errorf(c, "unknown escape '\\%c' (valid: \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH)",
                   s[i]);
            return -1;
        }
    }
    return o;
}

static void string(Compiler *c, bool can_assign) {
    (void)can_assign;
    int n = c->prev.length - 2;
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL) {
        error(c, "out of memory");
        return;
    }
    int len = decode_escapes(c, c->prev.start + 1, n, buf);
    if (len >= 0) {
        int idx = program_intern(&c->vm->prog, buf, (uint32_t)len);
        if (idx < 0) error(c, "too many different strings (max 65536)");
        else emit_const(c, const_str_val((uint16_t)idx));
    }
    free(buf);
}

static void char_literal(Compiler *c, bool can_assign) {
    (void)can_assign;
    char buf[8];
    int n = c->prev.length - 2;
    if (n < 1 || n > 4) {
        error(c, "a character literal holds exactly one character, like 'A' or '\\n'");
        return;
    }
    int len = decode_escapes(c, c->prev.start + 1, n, buf);
    if (len < 0) return;
    if (len != 1) {
        error(c, "a character literal holds exactly one character, like 'A' or '\\n'");
        return;
    }
    emit_const(c, int_val((uint8_t)buf[0]));
}

static void literal(Compiler *c, bool can_assign) {
    (void)can_assign;
    switch (c->prev.type) {
    case TOK_TRUE: emit(c, OP_TRUE); break;
    case TOK_FALSE: emit(c, OP_FALSE); break;
    default: emit(c, OP_NIL); break;
    }
}

/* After an error inside { ... }, skip to the closing brace so the rest of
   the block doesn't cause confusing follow-on errors. */
static void skip_block(Compiler *c) {
    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) advance(c);
    match(c, TOK_RBRACE);
}

/* The struct type called `name`, or -1. */
static int find_struct(Compiler *c, const char *name, int len) {
    const Program *p = &c->vm->prog;
    for (int i = 0; i < p->struct_count; i++) {
        const StrConst *s = &p->strs[p->structs[i].name];
        if ((int)s->len == len && memcmp(s->chars, name, (size_t)len) == 0) return i;
    }
    return -1;
}

static int find_field(const Program *p, const StructDef *d, const char *name, int len) {
    for (int i = 0; i < d->field_count; i++) {
        const StrConst *s = &p->strs[d->fields[i]];
        if ((int)s->len == len && memcmp(s->chars, name, (size_t)len) == 0) return i;
    }
    return -1;
}

/* "temp, time": a struct's fields, for error messages. */
static void field_list(const Program *p, const StructDef *d, char *out, size_t size) {
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < d->field_count && n + 1 < size; i++) {
        const StrConst *s = &p->strs[d->fields[i]];
        int w = snprintf(out + n, size - n, "%s%.*s", i ? ", " : "", (int)s->len, s->chars);
        if (w < 0) break;
        n += (size_t)w;
    }
}

/* Reading { temp: 21, time: 0 }: a new struct on the heap. */
static void struct_literal(Compiler *c, int type) {
    Program *p = &c->vm->prog;
    const StructDef *d = &p->structs[type];
    const StrConst *sname = &p->strs[d->name];
    emit(c, OP_STRUCT);
    emit(c, (uint8_t)(type >> 8));
    emit(c, (uint8_t)type);
    bool given[JL_FIELDS_MAX] = {false};
    consume(c, TOK_LBRACE, "expected '{'");
    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) {
        consume(c, TOK_IDENT, "expected a field name, like: temp: 21");
        Token f = c->prev;
        int slot = find_field(p, d, f.start, f.length);
        if (slot < 0) {
            char fields[256];
            field_list(p, d, fields, sizeof fields);
            char msg[400];
            snprintf(msg, sizeof msg, "%.*s has no field '%.*s' (its fields are %s)", (int)sname->len, sname->chars,
                     f.length, f.start, fields);
            error(c, msg);
            skip_block(c);
            return;
        }
        if (given[slot]) {
            error(c, "this field is given twice");
            skip_block(c);
            return;
        }
        given[slot] = true;
        consume(c, TOK_COLON, "expected ':' after the field name, like: temp: 21");
        expression(c);
        emit2(c, OP_INIT_FIELD, (uint8_t)slot);
        if (!match(c, TOK_COMMA)) break;
    }
    consume(c, TOK_RBRACE, "expected '}' after the fields");
    for (int i = 0; i < d->field_count; i++) {
        if (!given[i]) {
            char msg[200];
            const StrConst *s = &p->strs[d->fields[i]];
            snprintf(msg, sizeof msg, "%.*s { ... } is missing the field '%.*s'", (int)sname->len, sname->chars,
                     (int)s->len, s->chars);
            error(c, msg);
            return;
        }
    }
}

/* value.field, value.field = x and value.field += x */
static void field_(Compiler *c, bool can_assign) {
    consume(c, TOK_IDENT, "expected a field name after '.'");
    Token f = c->prev;
    int si = program_intern(&c->vm->prog, f.start, (uint32_t)f.length);
    if (si < 0 || si > 0xFFFF) {
        error(c, "too many names in one program");
        return;
    }
    if (can_assign && match(c, TOK_EQ)) {
        expression(c);
        emit(c, OP_SET_FIELD);
    } else if (can_assign && is_compound(c->cur.type)) {
        TokenType op = c->cur.type;
        advance(c);
        emit(c, OP_DUP);
        emit(c, OP_GET_FIELD);
        emit(c, (uint8_t)(si >> 8));
        emit(c, (uint8_t)si);
        expression(c);
        emit(c, compound_op(op));
        emit(c, OP_SET_FIELD);
    } else {
        emit(c, OP_GET_FIELD);
    }
    emit(c, (uint8_t)(si >> 8));
    emit(c, (uint8_t)si);
}

/* The const called `name`, or -1. */
static int find_named(Compiler *c, const char *name, int len) {
    const Program *p = &c->vm->prog;
    for (int i = 0; i < p->named_count; i++) {
        const StrConst *s = &p->strs[p->named[i].name];
        if ((int)s->len == len && memcmp(s->chars, name, (size_t)len) == 0) return i;
    }
    return -1;
}

/* The enum called `name`, or -1. */
static int find_enum(Compiler *c, const char *name, int len) {
    const Program *p = &c->vm->prog;
    for (int i = 0; i < p->enum_count; i++) {
        const StrConst *s = &p->strs[p->enum_names[i]];
        if ((int)s->len == len && memcmp(s->chars, name, (size_t)len) == 0) return i;
    }
    return -1;
}

static bool find_enum_member(Compiler *c, uint16_t enum_si, const char *name, int len, int32_t *out) {
    const Program *p = &c->vm->prog;
    for (int i = 0; i < p->enum_member_count; i++) {
        if (p->enum_members[i].enum_name != enum_si) continue;
        const StrConst *s = &p->strs[p->enum_members[i].member_name];
        if ((int)s->len == len && memcmp(s->chars, name, (size_t)len) == 0) {
            if (out) *out = p->enum_members[i].value;
            return true;
        }
    }
    return false;
}

static void variable(Compiler *c, bool can_assign) {
    Token name = c->prev;
    if (resolve_local(c, &name) < 0) {
        int k = find_named(c, name.start, name.length);
        if (k >= 0) {
            if (can_assign && (check(c, TOK_EQ) || is_compound(c->cur.type))) {
                error_at_current(c, "this is a const, which can't be changed (use let for a variable)");
                return;
            }
            emit_const(c, int_val(c->vm->prog.named[k].value));
            return;
        }
        int e = find_enum(c, name.start, name.length);
        if (e >= 0) {
            uint16_t enum_si = c->vm->prog.enum_names[e];
            consume(c, TOK_DOT, "expected '.' and a member name after an enum name");
            if (c->panic) return;
            consume(c, TOK_IDENT, "expected an enum member name after '.'");
            if (c->panic) return;
            Token mem = c->prev;
            int32_t val = 0;
            if (!find_enum_member(c, enum_si, mem.start, mem.length, &val)) {
                errorf(c, "enum '%.*s' has no member '%.*s'", name.length, name.start, mem.length, mem.start);
                return;
            }
            if (can_assign && (check(c, TOK_EQ) || is_compound(c->cur.type))) {
                error_at_current(c, "enum values are constants and can't be changed");
                return;
            }
            emit_const(c, int_val(val));
            c->last_enum_end = chunk(c)->count;
            c->last_enum_si = enum_si;
            c->last_enum_mem_si = (uint16_t)program_intern(&c->vm->prog, mem.start, (uint32_t)mem.length);
            return;
        }
    }
    int type = find_struct(c, name.start, name.length);
    if (type >= 0 && check(c, TOK_LBRACE)) {
        struct_literal(c, type);
        return;
    }
    uint8_t get, set;
    int arg = resolve_local(c, &name);
    if (arg >= 0) {
        get = OP_GET_LOCAL;
        set = OP_SET_LOCAL;
    } else {
        arg = global_use(c, name);
        get = OP_GET_GLOBAL;
        set = OP_SET_GLOBAL;
    }

    if (can_assign && match(c, TOK_EQ)) {
        expression(c);
        emit2(c, set, (uint8_t)arg);
    } else if (can_assign && is_compound(c->cur.type)) {
        TokenType op = c->cur.type;
        advance(c);
        emit2(c, get, (uint8_t)arg);
        expression(c);
        emit(c, compound_op(op));
        emit2(c, set, (uint8_t)arg);
    } else {
        emit2(c, get, (uint8_t)arg);
        c->var_end = chunk(c)->count;
        c->var_set_op = set;
        c->var_arg = (uint8_t)arg;
    }
}

static void unary(Compiler *c, bool can_assign) {
    (void)can_assign;
    TokenType op = c->prev.type;
    parse_precedence(c, PREC_UNARY);
    switch (op) {
    case TOK_MINUS: emit(c, OP_NEG); break;
    case TOK_BANG: emit(c, OP_NOT); break;
    default: emit(c, OP_BNOT); break;
    }
}

static void binary(Compiler *c, bool can_assign) {
    (void)can_assign;
    TokenType op = c->prev.type;
    bool left_is_sum = c->add_chunk == chunk(c) && c->add_end == chunk(c)->count;
    parse_precedence(c, (Precedence)(get_rule(op)->prec + 1));
    switch (op) {
    case TOK_PLUS:
        emit(c, left_is_sum ? OP_ADD_TEMP : OP_ADD);
        c->add_end = chunk(c)->count;
        c->add_chunk = chunk(c);
        break;
    case TOK_MINUS: emit(c, OP_SUB); break;
    case TOK_STAR: emit(c, OP_MUL); break;
    case TOK_SLASH: emit(c, OP_DIV); break;
    case TOK_PERCENT: emit(c, OP_MOD); break;
    case TOK_AMP: emit(c, OP_BAND); break;
    case TOK_PIPE: emit(c, OP_BOR); break;
    case TOK_CARET: emit(c, OP_BXOR); break;
    case TOK_SHL: emit(c, OP_SHL); break;
    case TOK_SHR: emit(c, OP_SHR); break;
    case TOK_EQ_EQ: emit(c, OP_EQ); break;
    case TOK_BANG_EQ: emit(c, OP_NE); break;
    case TOK_LT: emit(c, OP_LT); break;
    case TOK_LE: emit(c, OP_LE); break;
    case TOK_GT: emit(c, OP_GT); break;
    case TOK_GE: emit(c, OP_GE); break;
    default: break;
    }
}

/* a && b: if a is false, skip b and produce false. */
static void and_(Compiler *c, bool can_assign) {
    (void)can_assign;
    int to_false = emit_jump(c, OP_JUMP_IF_FALSE);
    parse_precedence(c, (Precedence)(PREC_AND + 1));
    emit(c, OP_BOOL);
    int to_end = emit_jump(c, OP_JUMP);
    patch_jump(c, to_false);
    emit(c, OP_FALSE);
    patch_jump(c, to_end);
}

/* a || b: if a is true, skip b and produce true. */
static void or_(Compiler *c, bool can_assign) {
    (void)can_assign;
    int to_right = emit_jump(c, OP_JUMP_IF_FALSE);
    emit(c, OP_TRUE);
    int to_end = emit_jump(c, OP_JUMP);
    patch_jump(c, to_right);
    parse_precedence(c, (Precedence)(PREC_OR + 1));
    emit(c, OP_BOOL);
    patch_jump(c, to_end);
}

static void call(Compiler *c, bool can_assign) {
    (void)can_assign;
    int argc = 0;
    if (!check(c, TOK_RPAREN)) {
        do {
            expression(c);
            if (++argc > 255) error(c, "too many arguments (max 255)");
        } while (match(c, TOK_COMMA));
    }
    consume(c, TOK_RPAREN, "expected ')' after the arguments");
    emit2(c, OP_CALL, (uint8_t)argc);
}

/*
 * x[i]       byte i of a buf/str, or bit i of an int
 * x[hi..lo]  bits hi down to lo of an int (Verilog style)
 *
 * Assigning to x[i] or x[hi..lo] when x is a variable writes the
 * result back into x, so ints behave like hardware registers.
 */
static void index_(Compiler *c, bool can_assign) {
    bool is_var = chunk(c)->count == c->var_end;
    uint8_t var_set = c->var_set_op;
    uint8_t var_arg = c->var_arg;

    expression(c);
    bool slice = match(c, TOK_DOTDOT);
    if (slice) expression(c);
    consume(c, TOK_RBRACKET, slice ? "expected ']' after the bit range"
                                   : "expected ']' after the index");

    bool assign = can_assign && match(c, TOK_EQ);
    bool compound = !assign && can_assign && is_compound(c->cur.type);
    if (!assign && !compound) {
        emit(c, slice ? OP_SLICE_GET : OP_INDEX_GET);
        return;
    }

    if (slice) {
        if (compound) {
            error_at_current(c, "+= and friends don't work on bit ranges; "
                                "write reg[7..4] = value instead");
            return;
        }
        if (!is_var) {
            error(c, "you can only assign to the bits of a variable, like reg[7..4] = 5");
            return;
        }
        expression(c);
        emit(c, OP_SLICE_UPDATE);
        emit2(c, var_set, var_arg);
        return;
    }

    if (compound) {
        TokenType op = c->cur.type;
        advance(c);
        emit(c, OP_DUP2);
        emit(c, OP_INDEX_GET);
        expression(c);
        emit(c, compound_op(op));
    } else {
        expression(c);
    }

    if (is_var) {
        emit(c, OP_INDEX_UPDATE);
        emit2(c, var_set, var_arg);
    } else {
        emit(c, OP_INDEX_SET);
    }
}

/* [a, b, c]: an array on the heap. A trailing comma is allowed. */
static void array_literal(Compiler *c, bool can_assign) {
    (void)can_assign;
    int n = 0;
    while (!check(c, TOK_RBRACKET) && !check(c, TOK_EOF)) {
        expression(c);
        if (++n > 65535) error(c, "too many items in one array literal (max 65535); use array(n) instead");
        if (!match(c, TOK_COMMA)) break;
    }
    consume(c, TOK_RBRACKET, "expected ']' after the array's items");
    emit(c, OP_ARRAY);
    emit(c, (uint8_t)(n >> 8));
    emit(c, (uint8_t)n);
}

static const ParseRule rules[TOK_COUNT] = {
    [TOK_LPAREN] = {grouping, call, PREC_CALL},
    [TOK_LBRACKET] = {array_literal, index_, PREC_CALL},
    [TOK_DOT] = {NULL, field_, PREC_CALL},
    [TOK_MINUS] = {unary, binary, PREC_TERM},
    [TOK_PLUS] = {NULL, binary, PREC_TERM},
    [TOK_STAR] = {NULL, binary, PREC_FACTOR},
    [TOK_SLASH] = {NULL, binary, PREC_FACTOR},
    [TOK_PERCENT] = {NULL, binary, PREC_FACTOR},
    [TOK_AMP] = {NULL, binary, PREC_BITAND},
    [TOK_PIPE] = {NULL, binary, PREC_BITOR},
    [TOK_CARET] = {NULL, binary, PREC_BITXOR},
    [TOK_SHL] = {NULL, binary, PREC_SHIFT},
    [TOK_SHR] = {NULL, binary, PREC_SHIFT},
    [TOK_TILDE] = {unary, NULL, PREC_NONE},
    [TOK_BANG] = {unary, NULL, PREC_NONE},
    [TOK_BANG_EQ] = {NULL, binary, PREC_EQUALITY},
    [TOK_EQ_EQ] = {NULL, binary, PREC_EQUALITY},
    [TOK_LT] = {NULL, binary, PREC_COMPARISON},
    [TOK_LE] = {NULL, binary, PREC_COMPARISON},
    [TOK_GT] = {NULL, binary, PREC_COMPARISON},
    [TOK_GE] = {NULL, binary, PREC_COMPARISON},
    [TOK_AND_AND] = {NULL, and_, PREC_AND},
    [TOK_OR_OR] = {NULL, or_, PREC_OR},
    [TOK_IDENT] = {variable, NULL, PREC_NONE},
    [TOK_STRING] = {string, NULL, PREC_NONE},
    [TOK_INT] = {number, NULL, PREC_NONE},
    [TOK_CHAR] = {char_literal, NULL, PREC_NONE},
    [TOK_TRUE] = {literal, NULL, PREC_NONE},
    [TOK_FALSE] = {literal, NULL, PREC_NONE},
    [TOK_NIL] = {literal, NULL, PREC_NONE},
};

static const ParseRule *get_rule(TokenType type) { return &rules[type]; }

/* ------------------------------------------------------------------ */
/* Statements                                                          */
/* ------------------------------------------------------------------ */

static void body(Compiler *c, const char *msg) {
    consume(c, TOK_LBRACE, msg);
    begin_scope(c);
    block(c);
    end_scope(c);
}

static void if_stmt(Compiler *c) {
    expression(c);
    int skip_then = emit_jump(c, OP_JUMP_IF_FALSE);
    body(c, "expected '{' after the if condition");
    if (match(c, TOK_ELSE)) {
        int skip_else = emit_jump(c, OP_JUMP);
        patch_jump(c, skip_then);
        if (match(c, TOK_IF)) {
            if_stmt(c);
        } else {
            body(c, "expected '{' or 'if' after 'else'");
        }
        patch_jump(c, skip_else);
    } else {
        patch_jump(c, skip_then);
    }
}

static void end_loop(Compiler *c, Loop *loop) {
    for (int i = 0; i < loop->break_count; i++) patch_jump(c, loop->breaks[i]);
    c->fs->loop = loop->enclosing;
}

static void while_stmt(Compiler *c) {
    Loop loop;
    memset(&loop, 0, sizeof loop);
    loop.enclosing = c->fs->loop;
    loop.start = chunk(c)->count;
    loop.depth = c->fs->scope_depth;

    expression(c);
    int exit = emit_jump(c, OP_JUMP_IF_FALSE);
    c->fs->loop = &loop;
    body(c, "expected '{' after the while condition");
    emit_loop(c, loop.start);
    patch_jump(c, exit);
    end_loop(c, &loop);
}

/* for i in start..end { body }   (end is exclusive) */
/* True if the code from `at` to the end is a single int constant. */
static bool const_int_at(Compiler *c, int at, int32_t *v) {
    Chunk *ch = chunk(c);
    if (ch->count != at + 3 || ch->code[at] != OP_CONST) return false;
    Value k = ch->consts[(ch->code[at + 1] << 8) | ch->code[at + 2]];
    if (k.type != VAL_INT) return false;
    *v = k.as.i;
    return true;
}

static void for_stmt(Compiler *c) {
    consume(c, TOK_IDENT, "expected a loop variable name, like: for i in 0..10");
    Token var = c->prev;
    consume(c, TOK_IN, "expected 'in' after the loop variable, like: for i in 0..10");

    begin_scope(c);
    int from_at = chunk(c)->count;
    expression(c);
    declare_local(c, var);
    mark_initialized(c);
    int i_slot = c->fs->local_count - 1;
    int32_t from, to;
    bool known = const_int_at(c, from_at, &from);

    consume(c, TOK_DOTDOT, "expected '..' in the range, like: for i in 0..10");
    int to_at = chunk(c)->count;
    expression(c);
    known = known && const_int_at(c, to_at, &to);
    Token end_name = {TOK_IDENT, "(end)", 5, var.line, var.col};
    declare_local(c, end_name);
    mark_initialized(c);
    int end_slot = c->fs->local_count - 1;

    Loop loop;
    memset(&loop, 0, sizeof loop);
    loop.enclosing = c->fs->loop;
    loop.is_for = true;
    loop.depth = c->fs->scope_depth;
    loop.start = chunk(c)->count;

    emit2(c, OP_GET_LOCAL, (uint8_t)i_slot);
    emit2(c, OP_GET_LOCAL, (uint8_t)end_slot);
    emit(c, OP_LT);
    int exit = emit_jump(c, OP_JUMP_IF_FALSE);

    c->fs->loop = &loop;
    int body_at = chunk(c)->count;
    body(c, "expected '{' after the range");
    /* The body runs to - from times, unless it changes the loop variable. */
    for (int i = body_at; known && i + 1 < chunk(c)->count; i++)
        if (chunk(c)->code[i] == OP_SET_LOCAL && chunk(c)->code[i + 1] == (uint8_t)i_slot) known = false;

    for (int i = 0; i < loop.continue_count; i++) patch_jump(c, loop.continues[i]);
    emit2(c, OP_GET_LOCAL, (uint8_t)i_slot);
    emit_const(c, int_val(1));
    emit(c, OP_ADD);
    emit2(c, OP_SET_LOCAL, (uint8_t)i_slot);
    emit(c, OP_POP);
    if (known) {
        Chunk *ch = chunk(c);
        if (ch->bound_count == ch->bound_cap) {
            ch->bound_cap = ch->bound_cap ? ch->bound_cap * 2 : 4;
            ch->bounds = jl_xrealloc(ch->bounds, (size_t)ch->bound_cap * sizeof *ch->bounds);
        }
        ch->bounds[ch->bound_count].off = ch->count;
        ch->bounds[ch->bound_count++].count = to > from ? to - from : 0;
    }
    emit_loop(c, loop.start);

    patch_jump(c, exit);
    end_loop(c, &loop);
    end_scope(c);
}

static void return_stmt(Compiler *c) {
    if (c->in_defer) {
        error(c, "'return' can't be used inside a defer");
        return;
    }
    if (c->fs->is_script) {
        error(c, "'return' can only be used inside a function");
    }
    if (check(c, TOK_RBRACE)) {
        if (c->fs->defer_count > 0) emit_defers(c, 1, false);
        emit(c, OP_NIL);
        emit(c, OP_RETURN);
        return;
    }
    expression(c);
    if (c->fs->defer_count > 0) {
        begin_scope(c);
        Token ret_name = {TOK_IDENT, "(ret)", 5, c->prev.line, c->prev.col};
        declare_local(c, ret_name);
        mark_initialized(c);
        int ret_slot = c->fs->local_count - 1;
        emit_defers(c, 1, false);
        emit2(c, OP_GET_LOCAL, (uint8_t)ret_slot);
        emit(c, OP_RETURN);
        c->fs->local_count--;
        c->fs->scope_depth--;
        return;
    }
    emit(c, OP_RETURN);
}

static void break_stmt(Compiler *c) {
    if (c->in_defer) {
        error(c, "'break' can't be used inside a defer");
        return;
    }
    Loop *loop = c->fs->loop;
    if (loop == NULL) {
        error(c, "'break' can only be used inside a loop");
        return;
    }
    emit_defers(c, loop->depth + 1, false);
    pop_locals_deeper_than(c, loop->depth);
    if (loop->break_count == MAX_LOOP_JUMPS) {
        error(c, "too many 'break's in one loop");
        return;
    }
    loop->breaks[loop->break_count++] = emit_jump(c, OP_JUMP);
}

static void continue_stmt(Compiler *c) {
    if (c->in_defer) {
        error(c, "'continue' can't be used inside a defer");
        return;
    }
    Loop *loop = c->fs->loop;
    if (loop == NULL) {
        error(c, "'continue' can only be used inside a loop");
        return;
    }
    emit_defers(c, loop->depth + 1, false);
    pop_locals_deeper_than(c, loop->depth);
    if (!loop->is_for) {
        emit_loop(c, loop->start);
        return;
    }
    if (loop->continue_count == MAX_LOOP_JUMPS) {
        error(c, "too many 'continue's in one loop");
        return;
    }
    loop->continues[loop->continue_count++] = emit_jump(c, OP_JUMP);
}

/* defer stmt  or  defer { ... }: runs in LIFO order when the current scope exits. */
static void defer_stmt(Compiler *c) {
    if (c->in_defer) {
        error(c, "'defer' can't be nested inside another defer");
        return;
    }
    if (c->fs->defer_count == MAX_DEFERS) {
        error(c, "too many 'defer' statements in one function (max 64)");
        return;
    }
    if (check(c, TOK_EOF) || check(c, TOK_RBRACE)) {
        error_at_current(c, "expected a statement after 'defer', like: defer free(buf)");
        return;
    }
    DeferEntry d;
    d.lx = c->lx;
    d.cur = c->cur;
    d.prev = c->prev;
    d.src = c->src;
    d.file_path = c->file_path;
    d.depth = c->fs->scope_depth;

    if (check(c, TOK_LBRACE)) {
        int braces = 0;
        do {
            if (check(c, TOK_LBRACE)) braces++;
            else if (check(c, TOK_RBRACE)) braces--;
            advance(c);
        } while (braces > 0 && !check(c, TOK_EOF));
        if (braces > 0) {
            error(c, "expected '}' to close this defer block");
            return;
        }
        d.end = c->prev.start + c->prev.length;
    } else {
        int start_line = c->cur.line;
        int parens = 0, brackets = 0;
        while (!check(c, TOK_EOF)) {
            if (parens == 0 && brackets == 0 && (c->cur.line != start_line || check(c, TOK_RBRACE))) break;
            if (check(c, TOK_LPAREN)) parens++;
            else if (check(c, TOK_RPAREN) && parens > 0) parens--;
            else if (check(c, TOK_LBRACKET)) brackets++;
            else if (check(c, TOK_RBRACKET) && brackets > 0) brackets--;
            advance(c);
        }
        d.end = c->prev.start + c->prev.length;
    }
    c->fs->defers[c->fs->defer_count++] = d;
}

/* match expr { 0 => { ... } 1, 2 => { ... } _ => { ... } } */
static void match_stmt(Compiler *c) {
    Token tok = c->prev;
    begin_scope(c);
    expression(c);
    Token slot_name = {TOK_IDENT, "(match)", 7, tok.line, tok.col};
    declare_local(c, slot_name);
    mark_initialized(c);
    int subject_slot = c->fs->local_count - 1;

    consume(c, TOK_LBRACE, "expected '{' after the match value");
    int ends[256];
    int end_count = 0;
    bool saw_default = false;
    int match_enum_si = -1;
    bool mixed_patterns = false;
    bool seen_member[256] = {false};

    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) {
        if (saw_default) {
            error_at_current(c, "'_' must be the last arm in a match");
            int braces = 0;
            while (!check(c, TOK_EOF)) {
                if (check(c, TOK_LBRACE)) braces++;
                else if (check(c, TOK_RBRACE)) {
                    if (braces == 0) break;
                    braces--;
                }
                advance(c);
            }
            break;
        }
        if (c->cur.type == TOK_IDENT && c->cur.length == 1 && c->cur.start[0] == '_') {
            advance(c);
            saw_default = true;
            if (match(c, TOK_COMMA)) {
                error(c, "'_' can't be combined with other values in a match arm");
                break;
            }
            consume(c, TOK_ARROW, "expected '=>' after '_'");
            body(c, "expected '{' after '=>'");
            match(c, TOK_COMMA);
            continue;
        }
        int hits[64];
        int hit_count = 0;
        for (;;) {
            emit2(c, OP_GET_LOCAL, (uint8_t)subject_slot);
            Token pat_tok = c->cur;
            c->last_enum_end = -1;
            expression(c);
            if (!c->had_error && c->last_enum_end == chunk(c)->count) {
                if (match_enum_si < 0) match_enum_si = (int)c->last_enum_si;
                else if (match_enum_si != (int)c->last_enum_si) mixed_patterns = true;
                if (!mixed_patterns) {
                    const Program *p = &c->vm->prog;
                    int ord = 0;
                    for (int m = 0; m < p->enum_member_count; m++) {
                        if (p->enum_members[m].enum_name != c->last_enum_si) continue;
                        if (p->enum_members[m].member_name == c->last_enum_mem_si) {
                            if (ord < 256) {
                                if (seen_member[ord]) {
                                    char dup_msg[160];
                                    snprintf(dup_msg, sizeof dup_msg, "duplicate match arm for '%s.%s'",
                                             p->strs[c->last_enum_si].chars, p->strs[c->last_enum_mem_si].chars);
                                    error_at(c, &pat_tok, dup_msg);
                                }
                                seen_member[ord] = true;
                            }
                            break;
                        }
                        ord++;
                    }
                }
            } else {
                mixed_patterns = true;
            }
            emit(c, OP_EQ);
            if (match(c, TOK_COMMA)) {
                int next_pat = emit_jump(c, OP_JUMP_IF_FALSE);
                if (hit_count < 64) hits[hit_count++] = emit_jump(c, OP_JUMP);
                else error(c, "too many values in one match arm (max 64)");
                patch_jump(c, next_pat);
                continue;
            }
            break;
        }
        int skip_arm = emit_jump(c, OP_JUMP_IF_FALSE);
        for (int i = 0; i < hit_count; i++) patch_jump(c, hits[i]);
        consume(c, TOK_ARROW, "expected '=>' after the match value");
        body(c, "expected '{' after '=>'");
        if (end_count < 256) ends[end_count++] = emit_jump(c, OP_JUMP);
        else error(c, "too many arms in one match (max 256)");
        patch_jump(c, skip_arm);
        match(c, TOK_COMMA);
    }

    consume(c, TOK_RBRACE, "expected '}' to close this match");
    if (!c->had_error && match_enum_si >= 0 && !mixed_patterns && !saw_default) {
        const Program *p = &c->vm->prog;
        char missing[256];
        size_t pos = 0;
        int miss_count = 0;
        int ord = 0;
        for (int m = 0; m < p->enum_member_count; m++) {
            if (p->enum_members[m].enum_name != (uint16_t)match_enum_si) continue;
            if (ord < 256 && !seen_member[ord]) {
                if (miss_count < 4 && pos + 4 < sizeof missing) {
                    int w = snprintf(missing + pos, sizeof missing - pos, "%s%s.%s",
                                     miss_count > 0 ? ", " : "",
                                     p->strs[match_enum_si].chars,
                                     p->strs[p->enum_members[m].member_name].chars);
                    if (w > 0) pos += (size_t)w;
                }
                miss_count++;
            }
            ord++;
        }
        if (miss_count > 0) {
            char msg[384];
            if (miss_count > 4) {
                snprintf(msg, sizeof msg,
                         "non-exhaustive match on enum '%s': missing %s and %d more (add arms or a '_' catch-all)",
                         p->strs[match_enum_si].chars, missing, miss_count - 4);
            } else {
                snprintf(msg, sizeof msg,
                         "non-exhaustive match on enum '%s': missing %s (add arms or a '_' catch-all)",
                         p->strs[match_enum_si].chars, missing);
            }
            error_at(c, &tok, msg);
        }
    }
    for (int i = 0; i < end_count; i++) patch_jump(c, ends[i]);
    end_scope(c);
}

static void statement(Compiler *c) {
    if (match(c, TOK_IF)) {
        if_stmt(c);
    } else if (match(c, TOK_MATCH)) {
        match_stmt(c);
    } else if (match(c, TOK_DEFER)) {
        defer_stmt(c);
    } else if (match(c, TOK_WHILE)) {
        while_stmt(c);
    } else if (match(c, TOK_FOR)) {
        for_stmt(c);
    } else if (match(c, TOK_RETURN)) {
        return_stmt(c);
    } else if (match(c, TOK_BREAK)) {
        break_stmt(c);
    } else if (match(c, TOK_CONTINUE)) {
        continue_stmt(c);
    } else if (match(c, TOK_LBRACE)) {
        begin_scope(c);
        block(c);
        end_scope(c);
    } else {
        parse_precedence(c, PREC_ASSIGN);
        emit(c, OP_POP);
    }
}

static void block(Compiler *c) {
    if (++c->nesting > JL_NESTING_MAX) {
        error_at_current(c, "this is nested too deeply (more than " JL_STR(JL_NESTING_MAX) " levels)");
        while (!check(c, TOK_EOF)) advance(c);
        c->nesting--;
        return;
    }
    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) declaration(c);
    consume(c, TOK_RBRACE, "expected '}' to close this block");
    c->nesting--;
}

static bool at_top_level(const Compiler *c);

/* Parses a compile-time integer constant: an int literal, a const name, or
   an Enum.MEMBER value, with optional leading '-'. */
static bool parse_const_int(Compiler *c, int32_t *out, const char *err_msg) {
    bool neg = match(c, TOK_MINUS);
    int32_t v = 0;
    if (match(c, TOK_INT)) {
        /* compile the number into a scratch spot, then take it back out */
        int at = chunk(c)->count;
        int consts = chunk(c)->const_count;
        number(c, false);
        if (chunk(c)->count == at + 3) {
            v = chunk(c)->consts[(chunk(c)->code[at + 1] << 8) | chunk(c)->code[at + 2]].as.i;
            chunk(c)->count = at;
            chunk(c)->const_count = consts;
        }
    } else if (match(c, TOK_IDENT)) {
        Token tok = c->prev;
        int k = find_named(c, tok.start, tok.length);
        if (k >= 0) {
            v = c->vm->prog.named[k].value;
        } else {
            int e = find_enum(c, tok.start, tok.length);
            if (e < 0) {
                error(c, err_msg);
                return false;
            }
            uint16_t enum_si = c->vm->prog.enum_names[e];
            consume(c, TOK_DOT, "expected '.' and a member name after an enum name");
            if (c->panic) return false;
            consume(c, TOK_IDENT, "expected an enum member name after '.'");
            if (c->panic) return false;
            if (!find_enum_member(c, enum_si, c->prev.start, c->prev.length, &v)) {
                errorf(c, "enum '%.*s' has no member '%.*s'", tok.length, tok.start, c->prev.length, c->prev.start);
                return false;
            }
        }
    } else {
        error_at_current(c, err_msg);
        return false;
    }
    if (neg) v = (int32_t)(0u - (uint32_t)v);
    *out = v;
    return true;
}

/* const NAME = 42: a number with a name. The compiler puts the number
   wherever the name is used, so it takes no memory, like #define in C. */
static void const_decl(Compiler *c) {
    if (!at_top_level(c)) error(c, "consts can only be declared at the top level of a file");
    consume(c, TOK_IDENT, "expected a name after 'const', like: const LED = 2");
    Token name = c->prev;
    if (find_named(c, name.start, name.length) >= 0 || find_struct(c, name.start, name.length) >= 0 ||
        find_enum(c, name.start, name.length) >= 0) {
        error(c, "this name is already taken");
        return;
    }
    consume(c, TOK_EQ, "expected '=' and a number, like: const LED = 2");
    int32_t v = 0;
    if (!parse_const_int(c, &v, "a const must be a number, another const, or an enum value")) return;
    Program *p = &c->vm->prog;
    if (p->named_count == p->named_cap) {
        p->named_cap = p->named_cap ? p->named_cap * 2 : 64;
        p->named = jl_xrealloc(p->named, (size_t)p->named_cap * sizeof *p->named);
    }
    p->named[p->named_count].name = (uint16_t)program_intern(p, name.start, (uint32_t)name.length);
    p->named[p->named_count++].value = v;
}

/* enum Mode { BUS, STUDY = 10, ASK }: scoped zero-RAM integer constants. */
static void enum_decl(Compiler *c) {
    if (!at_top_level(c)) error(c, "enums can only be declared at the top level of a file");
    consume(c, TOK_IDENT, "expected a name after 'enum', like: enum Mode { BUS, STUDY }");
    Token name = c->prev;
    if (find_named(c, name.start, name.length) >= 0 || find_struct(c, name.start, name.length) >= 0 ||
        find_enum(c, name.start, name.length) >= 0) {
        error(c, "this name is already taken");
        return;
    }
    int gi = vm_global_find(c->vm, name.start, name.length);
    if (gi >= 0 && c->vm->globals[gi].declared) {
        error(c, "this name is already taken");
        return;
    }
    Program *p = &c->vm->prog;
    int enum_si_i = program_intern(p, name.start, (uint32_t)name.length);
    if (enum_si_i < 0 || enum_si_i > 0xFFFF) {
        error(c, "too many names in one program");
        return;
    }
    uint16_t enum_si = (uint16_t)enum_si_i;
    consume(c, TOK_LBRACE, "expected '{' and then the enum member names");
    int32_t next_val = 0;
    int count = 0;
    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) {
        consume(c, TOK_IDENT, "expected an enum member name");
        if (c->panic) {
            skip_block(c);
            return;
        }
        Token mem = c->prev;
        if (find_enum_member(c, enum_si, mem.start, mem.length, NULL)) {
            errorf(c, "enum member '%.*s' is listed twice", mem.length, mem.start);
            skip_block(c);
            return;
        }
        int32_t val = next_val;
        if (match(c, TOK_EQ)) {
            if (!parse_const_int(c, &val, "an enum value must be a number, const, or enum value")) {
                skip_block(c);
                return;
            }
        }
        int mem_si_i = program_intern(p, mem.start, (uint32_t)mem.length);
        if (mem_si_i < 0 || mem_si_i > 0xFFFF) {
            error(c, "too many names in one program");
            skip_block(c);
            return;
        }
        if (p->enum_member_count == p->enum_member_cap) {
            p->enum_member_cap = p->enum_member_cap ? p->enum_member_cap * 2 : 32;
            p->enum_members = jl_xrealloc(p->enum_members, (size_t)p->enum_member_cap * sizeof *p->enum_members);
        }
        p->enum_members[p->enum_member_count].enum_name = enum_si;
        p->enum_members[p->enum_member_count].member_name = (uint16_t)mem_si_i;
        p->enum_members[p->enum_member_count++].value = val;
        next_val = (int32_t)((uint32_t)val + 1u);
        count++;
        match(c, TOK_COMMA);
    }
    consume(c, TOK_RBRACE, "expected '}' after the enum members");
    if (count == 0) {
        error(c, "an enum needs at least one member");
        return;
    }
    if (p->enum_count == p->enum_cap) {
        p->enum_cap = p->enum_cap ? p->enum_cap * 2 : 16;
        p->enum_names = jl_xrealloc(p->enum_names, (size_t)p->enum_cap * sizeof *p->enum_names);
    }
    p->enum_names[p->enum_count++] = enum_si;
}

static void let_decl(Compiler *c) {
    consume(c, TOK_IDENT, "expected a variable name after 'let'");
    Token name = c->prev;
    FnState *fs = c->fs;
    if (fs->is_script && fs->scope_depth == 0) {
        int g = global_declare(c, name);
        if (match(c, TOK_EQ)) expression(c);
        else emit(c, OP_NIL);
        emit2(c, OP_DEF_GLOBAL, (uint8_t)g);
    } else {
        declare_local(c, name);
        if (match(c, TOK_EQ)) expression(c);
        else emit(c, OP_NIL);
        mark_initialized(c);
    }
}

static int compile_function(Compiler *c, const char *name, int len, bool has_params) {
    int fi = program_add_fn(&c->vm->prog, name, len);
    if (fi < 0) {
        error(c, "too many functions");
        return -1;
    }
    FnState *fs = calloc(1, sizeof *fs);
    if (fs == NULL) {
        error(c, "out of memory");
        return -1;
    }
    fs->enclosing = c->fs;
    fs->fn = fi;
    fs->local_count = 1; /* slot 0 holds the function being called */
    fs->locals[0].name.start = "";
    c->fs = fs;
    c->var_end = -1;
    begin_scope(c);

    int arity = 0;
    if (has_params) {
        consume(c, TOK_LPAREN, "expected '(' after the function name");
        if (!check(c, TOK_RPAREN)) {
            do {
                if (++arity > 255) error_at_current(c, "too many parameters (max 255)");
                consume(c, TOK_IDENT, "expected a parameter name");
                declare_local(c, c->prev);
                mark_initialized(c);
            } while (match(c, TOK_COMMA));
        }
        consume(c, TOK_RPAREN, "expected ')' after the parameters");
    }
    c->vm->prog.fns[fi].arity = arity;

    consume(c, TOK_LBRACE, has_params ? "expected '{' to start the function body"
                                      : "expected '{' to start the block");
    block(c);
    emit_defers(c, 1, true);
    emit(c, OP_NIL);
    emit(c, OP_RETURN);

    c->fs = fs->enclosing;
    c->var_end = -1;
    free(fs);
    return fi;
}

static bool at_top_level(const Compiler *c) {
    return c->fs->is_script && c->fs->scope_depth == 0;
}

static void fn_decl(Compiler *c) {
    if (!at_top_level(c)) error(c, "functions can only be declared at the top level of a file");
    consume(c, TOK_IDENT, "expected a function name after 'fn'");
    Token name = c->prev;
    int g = global_declare(c, name);
    int fi = compile_function(c, name.start, name.length, true);
    if (fi < 0) return;
    emit_const(c, fn_val((uint16_t)fi));
    emit2(c, OP_DEF_GLOBAL, (uint8_t)g);
}

/* on button1 { ... }  - runs whenever the host fires "button1". */
static void on_decl(Compiler *c) {
    if (!at_top_level(c)) error(c, "event handlers can only be declared at the top level of a file");
    consume(c, TOK_IDENT, "expected an event name after 'on', like: on button1 { ... }");
    Token ev = c->prev;
    if (ev.length > 31) {
        error(c, "event names can be at most 31 characters long");
        return;
    }
    char name[40];
    snprintf(name, sizeof name, "on %.*s", ev.length, ev.start);
    int fi = compile_function(c, name, (int)strlen(name), false);
    int si = program_intern(&c->vm->prog, ev.start, (uint32_t)ev.length);
    if (fi < 0 || si < 0) return;
    emit_const(c, const_str_val((uint16_t)si));
    emit_const(c, fn_val((uint16_t)fi));
    emit(c, OP_ON);
}

/* Decode a TOK_STRING into a C buffer (strips quotes and handles \\ and \"). */
static int decode_string_tok(const Token *t, char *out, size_t size) {
    if (t->length < 2 || size == 0) return -1;
    const char *s = t->start + 1;
    int raw_len = t->length - 2;
    size_t n = 0;
    for (int i = 0; i < raw_len; i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < raw_len) {
            i++;
            switch (s[i]) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            default: ch = s[i]; break;
            }
        }
        if (n + 1 >= size) return -1;
        out[n++] = ch;
    }
    out[n] = '\0';
    return (int)n;
}

/* test "name" { ... }: unit test block. Skipped by `jlang run`/`build`,
   run in isolation with leak checking by `jlang test`. */
static void test_decl(Compiler *c) {
    if (!at_top_level(c)) error(c, "tests can only be declared at the top level of a file");
    consume(c, TOK_STRING, "expected a test name string after 'test', like: test \"adds numbers\" { ... }");
    if (c->panic) return;
    char tname[128];
    int tlen = decode_string_tok(&c->prev, tname, sizeof tname);
    if (tlen <= 0) {
        error(c, "a test name must be a non-empty string (at most 127 characters)");
        return;
    }
    /* Tests inside imported `use` files belong to that file, not the caller. */
    if (c->in_use) {
        consume(c, TOK_LBRACE, "expected '{' to start the test block");
        if (c->panic) return;
        int braces = 1;
        while (braces > 0 && !check(c, TOK_EOF)) {
            if (check(c, TOK_LBRACE)) braces++;
            else if (check(c, TOK_RBRACE)) braces--;
            advance(c);
        }
        if (braces > 0) error(c, "expected '}' to close this test block");
        return;
    }
    char fn_name[144];
    snprintf(fn_name, sizeof fn_name, "test: %s", tname);
    int fi = compile_function(c, fn_name, (int)strlen(fn_name), false);
    if (fi < 0) return;
    Program *p = &c->vm->prog;
    p->fns[fi].is_test = true;
    int si = program_intern(p, tname, (uint32_t)tlen);
    if (si < 0 || si > 0xFFFF) {
        error(c, "too many strings in one program");
        return;
    }
    if (p->test_count == p->test_cap) {
        p->test_cap = p->test_cap ? p->test_cap * 2 : 16;
        p->tests = jl_xrealloc(p->tests, (size_t)p->test_cap * sizeof *p->tests);
    }
    p->tests[p->test_count].name = (uint16_t)si;
    p->tests[p->test_count++].fn = fi;
}

/* Normalises a path with '/' separators, collapsing '.' and '..'. */
static bool normalize_path(const char *raw, char *out, size_t size) {
    char tmp[512];
    size_t rlen = strlen(raw);
    if (rlen >= sizeof tmp) return false;
    for (size_t i = 0; i <= rlen; i++) tmp[i] = raw[i] == '\\' ? '/' : raw[i];
    bool abs_path = tmp[0] == '/';
    const char *segs[128];
    int nseg = 0;
    char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (nseg > 0 && strcmp(segs[nseg - 1], "..") != 0) nseg--;
            else if (!abs_path && nseg < 128) segs[nseg++] = "..";
        } else if (nseg < 128) {
            segs[nseg++] = start;
        }
    }
    size_t pos = 0;
    if (abs_path) {
        if (pos + 2 > size) return false;
        out[pos++] = '/';
    }
    for (int i = 0; i < nseg; i++) {
        size_t sl = strlen(segs[i]);
        if (pos + sl + 2 > size) return false;
        if (i > 0) out[pos++] = '/';
        memcpy(out + pos, segs[i], sl);
        pos += sl;
    }
    if (pos == 0) {
        if (size < 2) return false;
        out[pos++] = '.';
    }
    out[pos] = '\0';
    return true;
}

static bool resolve_use_path(const char *base_path, const char *rel, char *out, size_t size) {
    char combined[512];
    if (rel[0] == '/' || rel[0] == '\\' || base_path == NULL) {
        if (strlen(rel) >= sizeof combined) return false;
        strcpy(combined, rel);
    } else {
        const char *slash = strrchr(base_path, '/');
        const char *bslash = strrchr(base_path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        if (slash != NULL) {
            size_t dir_len = (size_t)(slash - base_path) + 1;
            if (dir_len + strlen(rel) >= sizeof combined) return false;
            memcpy(combined, base_path, dir_len);
            strcpy(combined + dir_len, rel);
        } else {
            if (strlen(rel) >= sizeof combined) return false;
            strcpy(combined, rel);
        }
    }
    return normalize_path(combined, out, size);
}

/* use "relative/file.jlang": include-once module import with cycle detection. */
static void use_decl(Compiler *c) {
    if (!at_top_level(c)) {
        error(c, "'use' can only be written at the top level of a file");
        return;
    }
    consume(c, TOK_STRING, "expected a file path after 'use', like: use \"pins.jlang\"");
    if (c->panic) return;
    char rel[256];
    int rlen = decode_string_tok(&c->prev, rel, sizeof rel);
    if (rlen <= 0) {
        error(c, "expected a non-empty file path after 'use'");
        return;
    }
    if (c->vm->file_reader == NULL) {
        error(c, "'use' is only available when compiling from files");
        return;
    }
    char resolved[512];
    if (!resolve_use_path(c->base_path, rel, resolved, sizeof resolved)) {
        errorf(c, "file path '%s' is too long", rel);
        return;
    }
    for (int i = 0; i < c->use_depth; i++) {
        if (strcmp(c->use_stack[i], resolved) == 0) {
            errorf(c, "circular 'use' of '%s'", resolved);
            return;
        }
    }
    for (int i = 0; i < c->used_count; i++) {
        if (strcmp(c->used_files[i], resolved) == 0) return; /* include-once */
    }
    if (c->use_depth >= MAX_USE_DEPTH) {
        error(c, "nested 'use' imports are too deep (max 16)");
        return;
    }
    if (c->used_count >= MAX_USED_FILES) {
        error(c, "too many 'use' imports in one program (max 128)");
        return;
    }
    char *imported_src = c->vm->file_reader(c->vm->file_reader_ud, resolved);
    if (imported_src == NULL) {
        errorf(c, "can't read '%s'", resolved);
        return;
    }
    size_t plen = strlen(resolved);
    char *path_copy = jl_xrealloc(NULL, plen + 1);
    memcpy(path_copy, resolved, plen + 1);
    c->used_files[c->used_count++] = path_copy;
    c->owned_srcs[c->owned_src_count++] = imported_src;
    c->use_stack[c->use_depth++] = path_copy;

    const char *saved_src = c->src;
    const char *saved_file = c->file_path;
    const char *saved_base = c->base_path;
    bool saved_in_use = c->in_use;
    Lexer saved_lx = c->lx;
    Token saved_cur = c->cur;
    Token saved_prev = c->prev;
    int saved_var_end = c->var_end;

    c->src = imported_src;
    c->file_path = path_copy;
    c->base_path = path_copy;
    c->in_use = true;
    c->var_end = -1;
    lexer_init(&c->lx, imported_src);
    advance(c);
    while (!match(c, TOK_EOF)) declaration(c);

    c->use_depth--;
    c->src = saved_src;
    c->file_path = saved_file;
    c->base_path = saved_base;
    c->in_use = saved_in_use;
    c->lx = saved_lx;
    c->cur = saved_cur;
    c->prev = saved_prev;
    c->var_end = saved_var_end;
    c->panic = false;
}

/* struct Reading { temp, time }: a type with named fields. */
static void struct_decl(Compiler *c) {
    if (!at_top_level(c)) error(c, "structs can only be declared at the top level of a file");
    consume(c, TOK_IDENT, "expected a name after 'struct', like: struct Reading { temp, time }");
    Token name = c->prev;
    Program *p = &c->vm->prog;
    if (find_struct(c, name.start, name.length) >= 0 || find_enum(c, name.start, name.length) >= 0) {
        error(c, "there is already a struct with this name");
        return;
    }
    if (p->struct_count == JL_STRUCTS_MAX) {
        error(c, "too many structs in one program");
        return;
    }
    StructDef *d = &p->structs[p->struct_count];
    memset(d, 0, sizeof *d);
    d->name = (uint16_t)program_intern(p, name.start, (uint32_t)name.length);
    consume(c, TOK_LBRACE, "expected '{' and then the field names");
    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) {
        consume(c, TOK_IDENT, "expected a field name");
        Token f = c->prev;
        if (find_field(p, d, f.start, f.length) >= 0) {
            error(c, "this field is listed twice");
            skip_block(c);
            return;
        }
        if (d->field_count == JL_FIELDS_MAX) {
            error(c, "a struct can have at most 32 fields");
            skip_block(c);
            return;
        }
        d->fields[d->field_count++] = (uint16_t)program_intern(p, f.start, (uint32_t)f.length);
        match(c, TOK_COMMA); /* fields go on separate lines or between commas */
    }
    consume(c, TOK_RBRACE, "expected '}' after the fields");
    if (d->field_count == 0) {
        error(c, "a struct needs at least one field");
        return;
    }
    p->struct_count++;
}

/* every 500ms { ... } and after 2s { ... }: run code later, on a timer.
   The program's top level finishes first; then the board runs the timers. */
static void timer_decl(Compiler *c, bool repeat) {
    const char *kw = repeat ? "every" : "after";
    if (!at_top_level(c)) error(c, "timers can only be declared at the top level of a file");
    int line = c->prev.line;
    if (check(c, TOK_LBRACE)) {
        error_at_current(c, repeat ? "say how often, like: every 500ms { ... }" : "say how long to wait, like: after 2s { ... }");
        return;
    }
    expression(c);
    char name[40];
    snprintf(name, sizeof name, "%s (line %d)", kw, line);
    int fi = compile_function(c, name, (int)strlen(name), false);
    if (fi < 0) return;
    emit_const(c, fn_val((uint16_t)fi));
    /* Errors about the timer point at its first line, not its closing brace. */
    chunk_write(chunk(c), OP_TIMER, line);
    chunk_write(chunk(c), repeat ? 1 : 0, line);
}

static void synchronize(Compiler *c) {
    c->panic = false;
    while (!check(c, TOK_EOF)) {
        switch (c->cur.type) {
        case TOK_LET: case TOK_FN: case TOK_ON: case TOK_EVERY: case TOK_AFTER: case TOK_STRUCT: case TOK_CONST:
        case TOK_ENUM: case TOK_USE: case TOK_TEST:
        case TOK_IF: case TOK_MATCH: case TOK_DEFER: case TOK_WHILE:
        case TOK_FOR: case TOK_RETURN: case TOK_BREAK: case TOK_CONTINUE:
        case TOK_RBRACE: /* let the enclosing block close normally */
            return;
        default:
            advance(c);
        }
    }
}

static void declaration(Compiler *c) {
    if (match(c, TOK_LET)) {
        let_decl(c);
    } else if (match(c, TOK_FN)) {
        fn_decl(c);
    } else if (match(c, TOK_ON)) {
        on_decl(c);
    } else if (match(c, TOK_CONST)) {
        const_decl(c);
    } else if (match(c, TOK_ENUM)) {
        enum_decl(c);
    } else if (match(c, TOK_USE)) {
        use_decl(c);
    } else if (match(c, TOK_TEST)) {
        test_decl(c);
    } else if (match(c, TOK_STRUCT)) {
        struct_decl(c);
    } else if (match(c, TOK_EVERY)) {
        timer_decl(c, true);
    } else if (match(c, TOK_AFTER)) {
        timer_decl(c, false);
    } else {
        statement(c);
    }
    if (c->panic) synchronize(c);
}

/* ------------------------------------------------------------------ */
/* Undefined names, with "did you mean" suggestions                    */
/* ------------------------------------------------------------------ */

static int edit_distance(const char *a, int n, const char *b, int m) {
    if (n > 31 || m > 31) return 99;
    int d[32][32];
    for (int i = 0; i <= n; i++) d[i][0] = i;
    for (int j = 0; j <= m; j++) d[0][j] = j;
    for (int i = 1; i <= n; i++) {
        for (int j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int best = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < best) best = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + cost < best) best = d[i - 1][j - 1] + cost;
            d[i][j] = best;
        }
    }
    return d[n][m];
}

static void report_undefined(Compiler *c) {
    JlVM *vm = c->vm;
    for (int i = 0; i < vm->global_count; i++) {
        Global *g = &vm->globals[i];
        if (g->declared || g->use_line == 0) continue;
        c->had_error = true;
        if (g->use_file) {
            vm_errf(vm, "error (%s, line %d, col %d): '%s' is not defined",
                    g->use_file, g->use_line, g->use_col, g->name);
        } else {
            vm_errf(vm, "error (line %d, col %d): '%s' is not defined",
                    g->use_line, g->use_col, g->name);
        }

        const char *best = NULL;
        int best_d = 3;
        for (int j = 0; j < vm->global_count; j++) {
            if (!vm->globals[j].declared) continue;
            int d = edit_distance(g->name, g->len, vm->globals[j].name,
                                  vm->globals[j].len);
            if (d < best_d) {
                best_d = d;
                best = vm->globals[j].name;
            }
        }
        if (best) vm_errf(vm, " - did you mean '%s'?\n", best);
        else vm_errf(vm, " (declare it with let, or check the spelling)\n");
        show_source_in(vm, g->use_src ? g->use_src : c->src, g->use_line, g->use_col);
        g->use_line = 0;
        g->use_src = NULL;
        g->use_file = NULL;
    }
}

int jl_compile(JlVM *vm, const char *source) {
    /* If this source doesn't compile, its declarations must not stay
       behind: in the REPL, a line with an error and then the corrected
       line would otherwise say the name is already declared. */
    int globals_before = vm->global_count;
    int structs_before = vm->prog.struct_count;
    int named_before = vm->prog.named_count;
    int enums_before = vm->prog.enum_count;
    int enum_members_before = vm->prog.enum_member_count;
    int tests_before = vm->prog.test_count;
    Global *saved = malloc((size_t)(globals_before ? globals_before : 1) * sizeof *saved);
    if (saved) memcpy(saved, vm->globals, (size_t)globals_before * sizeof *saved);

    Compiler c;
    memset(&c, 0, sizeof c);
    c.vm = vm;
    c.src = source;
    c.var_end = -1;
    char root_norm[512];
    if (vm->source_path != NULL && normalize_path(vm->source_path, root_norm, sizeof root_norm)) {
        size_t rlen = strlen(root_norm);
        char *rcopy = jl_xrealloc(NULL, rlen + 1);
        memcpy(rcopy, root_norm, rlen + 1);
        c.base_path = rcopy;
        c.used_files[c.used_count++] = rcopy;
        c.use_stack[c.use_depth++] = rcopy;
    }
    lexer_init(&c.lx, source);

    int fi = program_add_fn(&vm->prog, "<script>", 8);
    FnState *fs = calloc(1, sizeof *fs);
    if (fi < 0 || fs == NULL) {
        free(fs);
        for (int i = 0; i < c.used_count; i++) free(c.used_files[i]);
        free(saved);
        vm_errf(vm, "error: out of memory\n");
        return -1;
    }
    fs->fn = fi;
    fs->is_script = true;
    fs->local_count = 1;
    fs->locals[0].name.start = "";
    c.fs = fs;

    advance(&c);
    while (!match(&c, TOK_EOF)) declaration(&c);
    emit_defers(&c, 0, true);
    emit(&c, OP_NIL);
    emit(&c, OP_RETURN);
    free(fs);

    if (!c.had_error) report_undefined(&c);
    for (int i = 0; i < vm->global_count; i++) {
        vm->globals[i].use_src = NULL;
        vm->globals[i].use_file = NULL;
    }
    if (c.errors > 10) vm_errf(vm, "... and %d more errors\n", c.errors - 10);
    if (c.had_error && saved) {
        for (int i = globals_before; i < vm->global_count; i++) free(vm->globals[i].name);
        vm->global_count = globals_before;
        memcpy(vm->globals, saved, (size_t)globals_before * sizeof *saved);
        vm->prog.struct_count = structs_before;
        vm->prog.named_count = named_before;
        vm->prog.enum_count = enums_before;
        vm->prog.enum_member_count = enum_members_before;
        vm->prog.test_count = tests_before;
    }
    for (int i = 0; i < c.owned_src_count; i++) free(c.owned_srcs[i]);
    for (int i = 0; i < c.used_count; i++) free(c.used_files[i]);
    free(saved);
    return c.had_error ? -1 : fi;
}
