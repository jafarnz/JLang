/*
 * fmt.c - zero-config source formatter for JLang.
 *
 * Uses the real lexer for every language token while preserving line and
 * block comments, single blank lines between sections, and single-line
 * braces (such as `Point { x: 1, y: 2 }` or `fn id(x) { return x }`).
 */
#include "fmt.h"
#include "lexer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ITEM_TOK,
    ITEM_LINE_COMMENT,
    ITEM_BLOCK_COMMENT,
} ItemKind;

typedef struct {
    ItemKind kind;
    TokenType type;
    const char *start;
    int length;
    int line;
    bool unary;
} Item;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} StrBuf;

static void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (sb->oom || n == 0) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2 : 512;
        while (ncap < sb->len + n + 1) ncap *= 2;
        char *nb = realloc(sb->data, ncap);
        if (nb == NULL) {
            sb->oom = true;
            return;
        }
        sb->data = nb;
        sb->cap = ncap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_putc(StrBuf *sb, char c) { sb_append_n(sb, &c, 1); }

static void sb_indent(StrBuf *sb, int level) {
    for (int i = 0; i < level * 4; i++) sb_putc(sb, ' ');
}

static bool ends_expr(TokenType t) {
    switch (t) {
    case TOK_IDENT:
    case TOK_INT:
    case TOK_CHAR:
    case TOK_STRING:
    case TOK_TRUE:
    case TOK_FALSE:
    case TOK_NIL:
    case TOK_RPAREN:
    case TOK_RBRACKET:
    case TOK_RBRACE:
        return true;
    default:
        return false;
    }
}

static bool is_binary_op(TokenType t) {
    switch (t) {
    case TOK_PLUS:
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_PERCENT:
    case TOK_AMP:
    case TOK_PIPE:
    case TOK_CARET:
    case TOK_SHL:
    case TOK_SHR:
    case TOK_EQ:
    case TOK_EQ_EQ:
    case TOK_BANG_EQ:
    case TOK_LT:
    case TOK_LE:
    case TOK_GT:
    case TOK_GE:
    case TOK_AND_AND:
    case TOK_OR_OR:
    case TOK_ARROW:
    case TOK_PLUS_EQ:
    case TOK_MINUS_EQ:
    case TOK_STAR_EQ:
    case TOK_SLASH_EQ:
    case TOK_PERCENT_EQ:
    case TOK_AMP_EQ:
    case TOK_PIPE_EQ:
    case TOK_CARET_EQ:
    case TOK_SHL_EQ:
    case TOK_SHR_EQ:
        return true;
    default:
        return false;
    }
}

static bool needs_space(const Item *prev, const Item *cur) {
    if (prev == NULL) return false;

    if (cur->kind == ITEM_LINE_COMMENT) return true;
    if (cur->kind == ITEM_BLOCK_COMMENT) return true;
    if (prev->kind == ITEM_BLOCK_COMMENT) {
        return cur->type != TOK_COMMA && cur->type != TOK_RPAREN &&
               cur->type != TOK_RBRACKET && cur->type != TOK_COLON &&
               cur->type != TOK_DOT;
    }

    TokenType a = prev->type;
    TokenType b = cur->type;

    /* Never put a space before closing/tight punctuation. */
    if (b == TOK_COMMA || b == TOK_COLON || b == TOK_RPAREN ||
        b == TOK_RBRACKET || b == TOK_DOT || b == TOK_DOTDOT)
        return false;

    /* Never put a space after opening/tight punctuation or unary ops. */
    if (a == TOK_LPAREN || a == TOK_LBRACKET || a == TOK_DOT ||
        a == TOK_DOTDOT || a == TOK_BANG || a == TOK_TILDE)
        return false;
    if (a == TOK_MINUS && prev->unary) return false;

    /* Empty braces stay `{}`. */
    if (a == TOK_LBRACE && b == TOK_RBRACE) return false;

    /* Function calls and indexing attach tightly to their callee/target. */
    if (b == TOK_LPAREN && (a == TOK_IDENT || a == TOK_RPAREN || a == TOK_RBRACKET))
        return false;
    if (b == TOK_LBRACKET && (a == TOK_IDENT || a == TOK_RPAREN || a == TOK_RBRACKET))
        return false;

    /* Braces on the same line get breathing room. */
    if (a == TOK_LBRACE || b == TOK_RBRACE) return true;
    if (b == TOK_LBRACE) return true;
    if (a == TOK_RBRACE) return true;

    /* Commas and colons always take a space after. */
    if (a == TOK_COMMA || a == TOK_COLON) return true;

    /* Binary operators and minus. */
    if (is_binary_op(a) || is_binary_op(b)) return true;
    if (a == TOK_MINUS || b == TOK_MINUS) return true;

    /* Unary ! and ~ get a space before them (already handled after (, [, !, ~, unary -). */
    if (b == TOK_BANG || b == TOK_TILDE) return true;

    /* Adjacent words, literals, keywords, or closing parens/brackets followed by a word. */
    return true;
}

static bool scan_items(const char *src, Item **out_items, int *out_count,
                       char *errbuf, int errbuf_size) {
    int cap = 128, count = 0;
    Item *items = malloc((size_t)cap * sizeof(Item));
    if (items == NULL) return false;

    const char *p = src;
    int line = 1;
    TokenType last_tok = TOK_EOF;
    int last_tok_line = 0;

    while (*p != '\0') {
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        if (*p == '\n') {
            line++;
            p++;
            continue;
        }
        if (count == cap) {
            cap *= 2;
            Item *bigger = realloc(items, (size_t)cap * sizeof(Item));
            if (bigger == NULL) {
                free(items);
                return false;
            }
            items = bigger;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *start = p;
            while (*p != '\0' && *p != '\n') p++;
            const char *end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
            Item it;
            it.kind = ITEM_LINE_COMMENT;
            it.type = TOK_EOF;
            it.start = start;
            it.length = (int)(end - start);
            it.line = line;
            it.unary = false;
            items[count++] = it;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *start = p;
            int start_line = line;
            p += 2;
            bool closed = false;
            while (*p != '\0') {
                if (p[0] == '*' && p[1] == '/') {
                    p += 2;
                    closed = true;
                    break;
                }
                if (*p == '\n') line++;
                p++;
            }
            if (!closed) {
                if (errbuf && errbuf_size > 0)
                    snprintf(errbuf, (size_t)errbuf_size, "line %d: unclosed block comment", start_line);
                free(items);
                return false;
            }
            Item it;
            it.kind = ITEM_BLOCK_COMMENT;
            it.type = TOK_EOF;
            it.start = start;
            it.length = (int)(p - start);
            it.line = start_line;
            it.unary = false;
            items[count++] = it;
            continue;
        }

        Lexer lx;
        lexer_init(&lx, p);
        lx.line = line;
        Token tok = lexer_next(&lx);
        if (tok.type == TOK_ERROR) {
            if (errbuf && errbuf_size > 0)
                snprintf(errbuf, (size_t)errbuf_size, "line %d: %s", tok.line, tok.start);
            free(items);
            return false;
        }
        if (tok.type == TOK_EOF) break;

        Item it;
        it.kind = ITEM_TOK;
        it.type = tok.type;
        it.start = tok.start;
        it.length = tok.length;
        it.line = tok.line;
        it.unary = (tok.type == TOK_MINUS) &&
                   !(last_tok_line == tok.line && ends_expr(last_tok));
        items[count++] = it;

        last_tok = tok.type;
        last_tok_line = tok.line;
        for (const char *q = p; q < lx.cur; q++) {
            if (*q == '\n') line++;
        }
        p = lx.cur;
    }

    *out_items = items;
    *out_count = count;
    return true;
}

static int end_line_of(const Item *it) {
    int line = it->line;
    for (int i = 0; i < it->length; i++) {
        if (it->start[i] == '\n') line++;
    }
    return line;
}

char *jl_format(const char *src, char *errbuf, int errbuf_size) {
    Item *items = NULL;
    int count = 0;
    if (!scan_items(src, &items, &count, errbuf, errbuf_size)) return NULL;

    StrBuf sb;
    memset(&sb, 0, sizeof sb);

    if (count == 0) {
        free(items);
        char *empty = calloc(1, 1);
        return empty;
    }

    int brace_depth = 0;
    int bracket_depth = 0;
    int prev_end_line = items[0].line;
    int idx = 0;

    while (idx < count) {
        int line = items[idx].line;
        int line_start = idx;
        int line_end = idx + 1;
        while (line_end < count && items[line_end].line == line) line_end++;

        /* Preserve a single blank line when the source had 1 or more. */
        if (line_start > 0 && line > prev_end_line + 1) {
            sb_putc(&sb, '\n');
        }

        /* Count leading closing delimiters on this line to dedent it. */
        int lead_rbrace = 0;
        int lead_rbracket = 0;
        for (int i = line_start; i < line_end; i++) {
            if (items[i].kind != ITEM_TOK) break;
            if (items[i].type == TOK_RBRACE) lead_rbrace++;
            else if (items[i].type == TOK_RBRACKET || items[i].type == TOK_RPAREN) lead_rbracket++;
            else break;
        }

        int cur_brace = brace_depth - lead_rbrace;
        if (cur_brace < 0) cur_brace = 0;
        int cur_bracket = bracket_depth - lead_rbracket;
        if (cur_bracket < 0) cur_bracket = 0;

        sb_indent(&sb, cur_brace + (cur_bracket > 0 ? 1 : 0));

        const Item *prev = NULL;
        int net_brace = 0;
        int net_bracket = 0;
        for (int i = line_start; i < line_end; i++) {
            const Item *cur = &items[i];
            if (prev != NULL) {
                if (cur->kind == ITEM_LINE_COMMENT) {
                    sb_append_n(&sb, "  ", 2);
                } else if (needs_space(prev, cur)) {
                    sb_putc(&sb, ' ');
                }
            }
            sb_append_n(&sb, cur->start, (size_t)cur->length);
            if (cur->kind == ITEM_TOK) {
                if (cur->type == TOK_LBRACE) net_brace++;
                else if (cur->type == TOK_RBRACE) net_brace--;
                else if (cur->type == TOK_LBRACKET || cur->type == TOK_LPAREN) net_bracket++;
                else if (cur->type == TOK_RBRACKET || cur->type == TOK_RPAREN) net_bracket--;
            }
            prev = cur;
        }
        sb_putc(&sb, '\n');

        brace_depth += net_brace;
        if (brace_depth < 0) brace_depth = 0;
        bracket_depth += net_bracket;
        if (bracket_depth < 0) bracket_depth = 0;

        prev_end_line = end_line_of(&items[line_end - 1]);
        idx = line_end;
    }

    free(items);
    if (sb.oom) {
        free(sb.data);
        if (errbuf && errbuf_size > 0) snprintf(errbuf, (size_t)errbuf_size, "out of memory");
        return NULL;
    }
    return sb.data;
}
