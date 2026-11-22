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

/* State for the function currently being compiled. */
typedef struct FnState {
    struct FnState *enclosing;
    int fn;
    bool is_script;
    Local locals[256];
    int local_count;
    int scope_depth;
    Loop *loop;
} FnState;

typedef struct {
    JlVM *vm;
    const char *src;
    Lexer lx;
    Token cur;
    Token prev;
    bool had_error;
    bool panic;
    int errors;
    FnState *fs;

    /* The most recent plain variable read. Lets `reg[3] = 1` write the
     * changed int back into the variable `reg`. */
    int var_end;
    uint8_t var_set_op;
    uint8_t var_arg;

    /* Where the most recent + ended, so a + chained onto it knows its left
     * value is a temporary string that can be freed after joining. */
    int add_end;
    Chunk *add_chunk;
} Compiler;

typedef void (*ParseFn)(Compiler *c, bool can_assign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence prec;
} ParseRule;

static void expression(Compiler *c);
static void parse_precedence(Compiler *c, Precedence prec);
static const ParseRule *get_rule(TokenType type);
static void declaration(Compiler *c);
static void block(Compiler *c);

/* ------------------------------------------------------------------ */
/* Error reporting                                                     */
/* ------------------------------------------------------------------ */

static const char *line_start_of(const Compiler *c, int line) {
    const char *p = c->src;
    for (int l = 1; *p && l < line; p++) {
        if (*p == '\n') l++;
    }
    return p;
}

/* Prints the source line and a ^ under the problem. */
static void show_source(Compiler *c, int line, int col) {
    const char *p = line_start_of(c, line);
    const char *e = p;
    while (*e && *e != '\n') e++;
    vm_errf(c->vm, "  %4d | %.*s\n", line, (int)(e - p), p);
    vm_errf(c->vm, "       | ");
    for (int i = 0; i < col - 1 && p + i < e; i++) {
        vm_write(c->vm, true, p[i] == '\t' ? "\t" : " ", 1);
    }
    vm_errf(c->vm, "^\n");
}

static void error_at(Compiler *c, const Token *t, const char *msg) {
    if (c->panic) return;
    c->panic = true;
    c->had_error = true;
    if (++c->errors > 10) return;
    if (t->type == TOK_EOF) {
        vm_errf(c->vm, "error (line %d): %s (reached the end of the file)\n",
                t->line, msg);
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

static void end_scope(Compiler *c) {
    FnState *fs = c->fs;
    fs->scope_depth--;
    while (fs->local_count > 0 &&
           fs->locals[fs->local_count - 1].depth > fs->scope_depth) {
        emit(c, OP_POP);
        fs->local_count--;
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
    c->fs->locals[c->fs->local_count - 1].depth = c->fs->scope_depth;
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
        if (v > 0xFFFFFFFFu) {
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
    emit_const(c, int_val((int32_t)(uint32_t)v));
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

static void variable(Compiler *c, bool can_assign) {
    Token name = c->prev;
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
static void for_stmt(Compiler *c) {
    consume(c, TOK_IDENT, "expected a loop variable name, like: for i in 0..10");
    Token var = c->prev;
    consume(c, TOK_IN, "expected 'in' after the loop variable, like: for i in 0..10");

    begin_scope(c);
    expression(c);
    declare_local(c, var);
    mark_initialized(c);
    int i_slot = c->fs->local_count - 1;

    consume(c, TOK_DOTDOT, "expected '..' in the range, like: for i in 0..10");
    expression(c);
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
    body(c, "expected '{' after the range");

    for (int i = 0; i < loop.continue_count; i++) patch_jump(c, loop.continues[i]);
    emit2(c, OP_GET_LOCAL, (uint8_t)i_slot);
    emit_const(c, int_val(1));
    emit(c, OP_ADD);
    emit2(c, OP_SET_LOCAL, (uint8_t)i_slot);
    emit(c, OP_POP);
    emit_loop(c, loop.start);

    patch_jump(c, exit);
    end_loop(c, &loop);
    end_scope(c);
}

static void return_stmt(Compiler *c) {
    if (c->fs->is_script) {
        error(c, "'return' can only be used inside a function");
    }
    if (check(c, TOK_RBRACE)) {
        emit(c, OP_NIL);
    } else {
        expression(c);
    }
    emit(c, OP_RETURN);
}

static void break_stmt(Compiler *c) {
    Loop *loop = c->fs->loop;
    if (loop == NULL) {
        error(c, "'break' can only be used inside a loop");
        return;
    }
    pop_locals_deeper_than(c, loop->depth);
    if (loop->break_count == MAX_LOOP_JUMPS) {
        error(c, "too many 'break's in one loop");
        return;
    }
    loop->breaks[loop->break_count++] = emit_jump(c, OP_JUMP);
}

static void continue_stmt(Compiler *c) {
    Loop *loop = c->fs->loop;
    if (loop == NULL) {
        error(c, "'continue' can only be used inside a loop");
        return;
    }
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

static void statement(Compiler *c) {
    if (match(c, TOK_IF)) {
        if_stmt(c);
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
    while (!check(c, TOK_RBRACE) && !check(c, TOK_EOF)) declaration(c);
    consume(c, TOK_RBRACE, "expected '}' to close this block");
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
                                      : "expected '{' to start the event handler");
    block(c);
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

static void synchronize(Compiler *c) {
    c->panic = false;
    while (!check(c, TOK_EOF)) {
        switch (c->cur.type) {
        case TOK_LET: case TOK_FN: case TOK_ON: case TOK_IF: case TOK_WHILE:
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
        vm_errf(vm, "error (line %d, col %d): '%s' is not defined",
                g->use_line, g->use_col, g->name);

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
        show_source(c, g->use_line, g->use_col);
        g->use_line = 0;
    }
}

int jl_compile(JlVM *vm, const char *source) {
    Compiler c;
    memset(&c, 0, sizeof c);
    c.vm = vm;
    c.src = source;
    c.var_end = -1;
    lexer_init(&c.lx, source);

    int fi = program_add_fn(&vm->prog, "<script>", 8);
    FnState *fs = calloc(1, sizeof *fs);
    if (fi < 0 || fs == NULL) {
        free(fs);
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
    emit(&c, OP_NIL);
    emit(&c, OP_RETURN);
    free(fs);

    if (!c.had_error) report_undefined(&c);
    if (c.errors > 10) vm_errf(vm, "... and %d more errors\n", c.errors - 10);
    return c.had_error ? -1 : fi;
}
