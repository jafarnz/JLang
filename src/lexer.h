/*
 * lexer.h - turns source text into tokens.
 *
 *   let x = 0xFF & mask
 *
 * becomes   LET  IDENT(x)  EQ  INT(0xFF)  AMP  IDENT(mask)
 *
 * The lexer never copies text: a token just points into the source.
 */
#ifndef JL_LEXER_H
#define JL_LEXER_H

#include "common.h"

typedef enum {
    /* punctuation */
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET, TOK_COMMA, TOK_DOTDOT, TOK_COLON,
    /* arithmetic */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    /* bitwise */
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_SHL, TOK_SHR,
    /* comparison and logic */
    TOK_BANG, TOK_BANG_EQ, TOK_EQ, TOK_EQ_EQ, TOK_ARROW,
    TOK_LT, TOK_LE, TOK_GT, TOK_GE, TOK_AND_AND, TOK_OR_OR,
    /* compound assignment */
    TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ, TOK_PERCENT_EQ,
    TOK_AMP_EQ, TOK_PIPE_EQ, TOK_CARET_EQ, TOK_SHL_EQ, TOK_SHR_EQ,
    /* literals */
    TOK_IDENT, TOK_STRING, TOK_INT, TOK_CHAR,
    /* keywords */
    TOK_LET, TOK_FN, TOK_RETURN, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR,
    TOK_IN, TOK_BREAK, TOK_CONTINUE, TOK_TRUE, TOK_FALSE, TOK_NIL, TOK_ON,
    TOK_EVERY, TOK_AFTER, TOK_STRUCT, TOK_DOT, TOK_CONST, TOK_MATCH,

    TOK_ERROR,
    TOK_EOF,
    TOK_COUNT
} TokenType;

typedef struct {
    TokenType type;
    const char *start; /* for TOK_ERROR: the error message */
    int length;
    int line;
    int col;
} Token;

typedef struct {
    const char *start;      /* start of the token being scanned */
    const char *cur;        /* next character to read */
    const char *line_start; /* first character of the current line */
    int line;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
Token lexer_next(Lexer *lx);

#endif
