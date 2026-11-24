#include <ctype.h>
#include <string.h>

#include "lexer.h"

void lexer_init(Lexer *lx, const char *src) {
    lx->start = src;
    lx->cur = src;
    lx->line_start = src;
    lx->limit = NULL;
    lx->line = 1;
}

static bool at_end(const Lexer *lx) {
    return *lx->cur == '\0' || (lx->limit != NULL && lx->cur >= lx->limit);
}
static char advance(Lexer *lx) { return *lx->cur++; }
static char peek(const Lexer *lx) { return at_end(lx) ? '\0' : *lx->cur; }
static char peek_next(const Lexer *lx) {
    if (at_end(lx)) return '\0';
    if (lx->limit != NULL && lx->cur + 1 >= lx->limit) return '\0';
    return lx->cur[1];
}

static bool match(Lexer *lx, char expected) {
    if (at_end(lx) || *lx->cur != expected) return false;
    lx->cur++;
    return true;
}

static Token make_token(const Lexer *lx, TokenType type) {
    Token t;
    t.type = type;
    t.start = lx->start;
    t.length = (int)(lx->cur - lx->start);
    t.line = lx->line;
    t.col = (int)(lx->start - lx->line_start) + 1;
    return t;
}

static Token error_token(const Lexer *lx, const char *message) {
    Token t;
    t.type = TOK_ERROR;
    t.start = message;
    t.length = (int)strlen(message);
    t.line = lx->line;
    t.col = (int)(lx->start - lx->line_start) + 1;
    return t;
}

/* Call right after consuming a '\n'. */
static void new_line(Lexer *lx) {
    lx->line++;
    lx->line_start = lx->cur;
}

/* Skips spaces and comments. Returns false on an unterminated comment. */
static bool skip_whitespace(Lexer *lx) {
    for (;;) {
        switch (peek(lx)) {
        case ' ':
        case '\t':
        case '\r':
            advance(lx);
            break;
        case '\n':
            advance(lx);
            new_line(lx);
            break;
        case '/':
            if (peek_next(lx) == '/') {
                while (peek(lx) != '\n' && !at_end(lx)) advance(lx);
            } else if (peek_next(lx) == '*') {
                const char *open = lx->cur;
                const char *open_line_start = lx->line_start;
                int open_line = lx->line;
                advance(lx);
                advance(lx);
                for (;;) {
                    if (at_end(lx)) {
                        /* Report the error where the comment started. */
                        lx->start = open;
                        lx->line_start = open_line_start;
                        lx->line = open_line;
                        return false;
                    }
                    if (peek(lx) == '*' && peek_next(lx) == '/') {
                        advance(lx);
                        advance(lx);
                        break;
                    }
                    if (advance(lx) == '\n') new_line(lx);
                }
            } else {
                return true;
            }
            break;
        default:
            return true;
        }
    }
}

static TokenType keyword_or_ident(const char *s, int n) {
    static const struct {
        const char *word;
        TokenType type;
    } keywords[] = {
        {"let", TOK_LET},       {"fn", TOK_FN},
        {"return", TOK_RETURN}, {"if", TOK_IF},
        {"else", TOK_ELSE},     {"while", TOK_WHILE},
        {"for", TOK_FOR},       {"in", TOK_IN},
        {"break", TOK_BREAK},   {"continue", TOK_CONTINUE},
        {"true", TOK_TRUE},     {"false", TOK_FALSE},
        {"nil", TOK_NIL},       {"on", TOK_ON},
        {"every", TOK_EVERY},   {"after", TOK_AFTER},
        {"struct", TOK_STRUCT}, {"const", TOK_CONST},
        {"match", TOK_MATCH},   {"defer", TOK_DEFER},
        {"enum", TOK_ENUM},     {"use", TOK_USE},
        {"test", TOK_TEST},
    };
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if ((int)strlen(keywords[i].word) == n &&
            memcmp(keywords[i].word, s, (size_t)n) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static Token identifier(Lexer *lx) {
    while (is_ident_char(peek(lx))) advance(lx);
    return make_token(lx, keyword_or_ident(lx->start, (int)(lx->cur - lx->start)));
}

/* 123, 1_000_000, 0xFF, 0b1010. The value is decoded by the compiler. */
static Token number(Lexer *lx, char first) {
    if (first == '0' && (peek(lx) == 'x' || peek(lx) == 'X')) {
        advance(lx);
        while (isxdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
    } else if (first == '0' && (peek(lx) == 'b' || peek(lx) == 'B')) {
        advance(lx);
        while (peek(lx) == '0' || peek(lx) == '1' || peek(lx) == '_') advance(lx);
    } else {
        while (isdigit((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        /* A time: 500ms or 2s. The compiler turns it into milliseconds. */
        const char *u = lx->cur;
        int ulen = 0;
        while (is_ident_char(u[ulen])) ulen++;
        if ((ulen == 2 && u[0] == 'm' && u[1] == 's') || (ulen == 1 && u[0] == 's')) {
            lx->cur += ulen;
            return make_token(lx, TOK_INT);
        }
    }
    if (is_ident_char(peek(lx))) {
        while (is_ident_char(peek(lx))) advance(lx);
        return error_token(lx, "invalid number literal (times are written 500ms or 2s)");
    }
    return make_token(lx, TOK_INT);
}

/* Shared by "strings" and 'c'haracters: scan up to the closing quote. */
static Token quoted(Lexer *lx, char quote, TokenType type) {
    while (peek(lx) != quote) {
        if (at_end(lx) || peek(lx) == '\n') {
            return error_token(lx, quote == '"'
                ? "unterminated string (a string must end with \" on the same line)"
                : "unterminated character literal");
        }
        if (peek(lx) == '\\' && peek_next(lx) != '\0' && peek_next(lx) != '\n') {
            advance(lx); /* skip the escaped character */
        }
        advance(lx);
    }
    advance(lx); /* closing quote */
    return make_token(lx, type);
}

Token lexer_next(Lexer *lx) {
    if (!skip_whitespace(lx)) {
        Token t = error_token(lx, "unterminated /* comment (missing */)");
        /* Leave the lexer at the end so the compiler stops. */
        while (!at_end(lx)) advance(lx);
        return t;
    }
    lx->start = lx->cur;
    if (at_end(lx)) return make_token(lx, TOK_EOF);

    char c = advance(lx);
    if (isalpha((unsigned char)c) || c == '_') return identifier(lx);
    if (isdigit((unsigned char)c)) return number(lx, c);

    switch (c) {
    case '(': return make_token(lx, TOK_LPAREN);
    case ')': return make_token(lx, TOK_RPAREN);
    case '{': return make_token(lx, TOK_LBRACE);
    case '}': return make_token(lx, TOK_RBRACE);
    case '[': return make_token(lx, TOK_LBRACKET);
    case ']': return make_token(lx, TOK_RBRACKET);
    case ',': return make_token(lx, TOK_COMMA);
    case '~': return make_token(lx, TOK_TILDE);
    case ':': return make_token(lx, TOK_COLON);
    case '.':
        if (match(lx, '.')) return make_token(lx, TOK_DOTDOT);
        return make_token(lx, TOK_DOT);
    case '+': return make_token(lx, match(lx, '=') ? TOK_PLUS_EQ : TOK_PLUS);
    case '-': return make_token(lx, match(lx, '=') ? TOK_MINUS_EQ : TOK_MINUS);
    case '*': return make_token(lx, match(lx, '=') ? TOK_STAR_EQ : TOK_STAR);
    case '/': return make_token(lx, match(lx, '=') ? TOK_SLASH_EQ : TOK_SLASH);
    case '%': return make_token(lx, match(lx, '=') ? TOK_PERCENT_EQ : TOK_PERCENT);
    case '^': return make_token(lx, match(lx, '=') ? TOK_CARET_EQ : TOK_CARET);
    case '&':
        if (match(lx, '&')) return make_token(lx, TOK_AND_AND);
        return make_token(lx, match(lx, '=') ? TOK_AMP_EQ : TOK_AMP);
    case '|':
        if (match(lx, '|')) return make_token(lx, TOK_OR_OR);
        return make_token(lx, match(lx, '=') ? TOK_PIPE_EQ : TOK_PIPE);
    case '!': return make_token(lx, match(lx, '=') ? TOK_BANG_EQ : TOK_BANG);
    case '=':
        if (match(lx, '=')) return make_token(lx, TOK_EQ_EQ);
        if (match(lx, '>')) return make_token(lx, TOK_ARROW);
        return make_token(lx, TOK_EQ);
    case '<':
        if (match(lx, '<')) return make_token(lx, match(lx, '=') ? TOK_SHL_EQ : TOK_SHL);
        return make_token(lx, match(lx, '=') ? TOK_LE : TOK_LT);
    case '>':
        if (match(lx, '>')) return make_token(lx, match(lx, '=') ? TOK_SHR_EQ : TOK_SHR);
        return make_token(lx, match(lx, '=') ? TOK_GE : TOK_GT);
    case '"': return quoted(lx, '"', TOK_STRING);
    case '\'': return quoted(lx, '\'', TOK_CHAR);
    }
    return error_token(lx, "unexpected character");
}
