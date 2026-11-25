/*
 * bundle.c - resolves `use "path.jlang" [as alias]` imports into one
 * standalone .jlang file for browser WASM and microcontroller deployment.
 */
#include "bundle.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define B_MAX_FILES 128
#define B_MAX_DEPTH 16
#define B_MAX_ALIASES 64
#define B_MAX_SYMS 256
#define B_MAX_LOCALS 256

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->oom || n == 0) return;
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 1024;
        while (ncap < b->len + n + 1) ncap *= 2;
        char *nb = realloc(b->data, ncap);
        if (nb == NULL) {
            b->oom = true;
            return;
        }
        b->data = nb;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

typedef struct {
    char alias[64];
    int mod_id;
} AliasEntry;

typedef struct {
    char name[64];
} SymName;

typedef struct {
    char *used_files[B_MAX_FILES];
    int used_mod_ids[B_MAX_FILES];
    int used_count;
    const char *stack[B_MAX_DEPTH];
    int depth;
    int next_mod_id;
    Buf out;
    char *errbuf;
    int errsize;
    bool failed;
} Bundler;

static void bfail(Bundler *b, const char *msg) {
    if (!b->failed) {
        b->failed = true;
        if (b->errbuf && b->errsize > 0) snprintf(b->errbuf, (size_t)b->errsize, "%s", msg);
    }
}

static char *read_entire_file(const char *path) {
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

static bool normalize_path(const char *in, char *out, size_t size) {
    char tmp[512];
    size_t len = strlen(in);
    if (len >= sizeof tmp) return false;
    for (size_t i = 0; i <= len; i++) tmp[i] = in[i] == '\\' ? '/' : in[i];

    bool abs_path = tmp[0] == '/';
    const char *parts[64];
    int nparts = 0;
    char *p = tmp;
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;
        char *start = p;
        while (*p != '\0' && *p != '/') p++;
        if (*p != '\0') *p++ = '\0';
        if (strcmp(start, ".") == 0) continue;
        if (strcmp(start, "..") == 0) {
            if (nparts > 0 && strcmp(parts[nparts - 1], "..") != 0) nparts--;
            else if (!abs_path && nparts < 64) parts[nparts++] = "..";
            continue;
        }
        if (nparts < 64) parts[nparts++] = start;
    }
    size_t o = 0;
    if (abs_path && o + 1 < size) out[o++] = '/';
    for (int i = 0; i < nparts; i++) {
        size_t plen = strlen(parts[i]);
        if (o + (i > 0 ? 1u : 0u) + plen >= size) return false;
        if (i > 0) out[o++] = '/';
        memcpy(out + o, parts[i], plen);
        o += plen;
    }
    if (o == 0 && size >= 2) out[o++] = '.';
    out[o] = '\0';
    return true;
}

static bool resolve_path(const char *base, const char *rel, char *out, size_t size) {
    char combined[512];
    if (rel[0] == '/' || rel[0] == '\\') {
        if (strlen(rel) >= sizeof combined) return false;
        strcpy(combined, rel);
    } else {
        const char *slash = strrchr(base, '/');
        if (slash != NULL) {
            size_t dlen = (size_t)(slash - base) + 1;
            if (dlen + strlen(rel) >= sizeof combined) return false;
            memcpy(combined, base, dlen);
            strcpy(combined + dlen, rel);
        } else {
            if (strlen(rel) >= sizeof combined) return false;
            strcpy(combined, rel);
        }
    }
    return normalize_path(combined, out, size);
}

static int decode_str(const Token *t, char *out, size_t size) {
    if (t->length < 2 || size == 0) return -1;
    const char *s = t->start + 1;
    int raw = t->length - 2;
    size_t n = 0;
    for (int i = 0; i < raw; i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < raw) {
            i++;
            if (s[i] == 'n') ch = '\n';
            else if (s[i] == 't') ch = '\t';
            else if (s[i] == 'r') ch = '\r';
            else ch = s[i];
        }
        if (n + 1 >= size) return -1;
        out[n++] = ch;
    }
    out[n] = '\0';
    return (int)n;
}

static bool tok_is_word(const Token *t, const char *w) {
    size_t n = strlen(w);
    return t->type == TOK_IDENT && (size_t)t->length == n && memcmp(t->start, w, n) == 0;
}

static bool has_sym(const SymName *list, int count, const char *s, int len) {
    for (int i = 0; i < count; i++) {
        if ((int)strlen(list[i].name) == len && memcmp(list[i].name, s, (size_t)len) == 0)
            return true;
    }
    return false;
}

static void add_sym(SymName *list, int *count, int cap, const char *s, int len) {
    if (*count >= cap || len <= 0 || len >= 64) return;
    if (has_sym(list, *count, s, len)) return;
    memcpy(list[*count].name, s, (size_t)len);
    list[*count].name[len] = '\0';
    (*count)++;
}

static int find_local_alias(const AliasEntry *aliases, int count, const char *s, int len) {
    for (int i = 0; i < count; i++) {
        if ((int)strlen(aliases[i].alias) == len && memcmp(aliases[i].alias, s, (size_t)len) == 0)
            return aliases[i].mod_id;
    }
    return -1;
}

static Token *lex_all(const char *src, int *out_count) {
    int cap = 128, n = 0;
    Token *toks = malloc((size_t)cap * sizeof(Token));
    if (toks == NULL) return NULL;
    Lexer lx;
    lexer_init(&lx, src);
    for (;;) {
        Token t = lexer_next(&lx);
        if (n == cap) {
            cap *= 2;
            Token *nb = realloc(toks, (size_t)cap * sizeof(Token));
            if (nb == NULL) {
                free(toks);
                return NULL;
            }
            toks = nb;
        }
        toks[n++] = t;
        if (t.type == TOK_EOF || t.type == TOK_ERROR) break;
    }
    *out_count = n;
    return toks;
}

static void bundle_file(Bundler *b, const char *norm_path, int mod_id);

static void emit_prefix(Buf *out, int mod_id) {
    char pfx[24];
    snprintf(pfx, sizeof pfx, "m%d__", mod_id);
    buf_str(out, pfx);
}

static void bundle_file(Bundler *b, const char *norm_path, int mod_id) {
    if (b->failed) return;
    for (int i = 0; i < b->depth; i++) {
        if (strcmp(b->stack[i], norm_path) == 0) {
            char msg[256];
            snprintf(msg, sizeof msg, "circular 'use' of '%s'", norm_path);
            bfail(b, msg);
            return;
        }
    }
    if (b->depth >= B_MAX_DEPTH) {
        bfail(b, "nested 'use' imports are too deep (max 16)");
        return;
    }
    if (b->used_count >= B_MAX_FILES) {
        bfail(b, "too many 'use' imports in one program (max 128)");
        return;
    }

    char *src = read_entire_file(norm_path);
    if (src == NULL) {
        char msg[256];
        snprintf(msg, sizeof msg, "can't read '%s'", norm_path);
        bfail(b, msg);
        return;
    }

    size_t plen = strlen(norm_path);
    char *path_copy = malloc(plen + 1);
    if (path_copy == NULL) {
        free(src);
        bfail(b, "out of memory");
        return;
    }
    memcpy(path_copy, norm_path, plen + 1);
    b->used_mod_ids[b->used_count] = mod_id;
    b->used_files[b->used_count++] = path_copy;
    b->stack[b->depth++] = path_copy;

    int ntoks = 0;
    Token *toks = lex_all(src, &ntoks);
    if (toks == NULL) {
        free(src);
        b->depth--;
        bfail(b, "out of memory");
        return;
    }
    if (ntoks > 0 && toks[ntoks - 1].type == TOK_ERROR) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s (line %d): %s", norm_path, toks[ntoks - 1].line, toks[ntoks - 1].start);
        free(toks);
        free(src);
        b->depth--;
        bfail(b, msg);
        return;
    }

    /* Pass 1: collect top-level declared names in this module (if namespaced)
       and resolve `use` imports in declaration order. */
    SymName top_syms[B_MAX_SYMS];
    int top_count = 0;
    AliasEntry aliases[B_MAX_ALIASES];
    int alias_count = 0;

    int brace = 0;
    for (int i = 0; i < ntoks && toks[i].type != TOK_EOF; i++) {
        TokenType t = toks[i].type;
        if (t == TOK_LBRACE) {
            brace++;
            continue;
        }
        if (t == TOK_RBRACE) {
            if (brace > 0) brace--;
            continue;
        }
        if (brace == 0) {
            if (mod_id > 0 &&
                (t == TOK_LET || t == TOK_FN || t == TOK_CONST || t == TOK_ENUM || t == TOK_STRUCT) &&
                i + 1 < ntoks && toks[i + 1].type == TOK_IDENT) {
                add_sym(top_syms, &top_count, B_MAX_SYMS, toks[i + 1].start, toks[i + 1].length);
            } else if (t == TOK_USE && i + 1 < ntoks && toks[i + 1].type == TOK_STRING) {
                char rel[256], resolved[512];
                if (decode_str(&toks[i + 1], rel, sizeof rel) <= 0 ||
                    !resolve_path(norm_path, rel, resolved, sizeof resolved)) {
                    bfail(b, "invalid file path in 'use'");
                    break;
                }
                bool has_alias = false;
                Token atok;
                memset(&atok, 0, sizeof atok);
                if (i + 3 < ntoks && tok_is_word(&toks[i + 2], "as") && toks[i + 3].type == TOK_IDENT) {
                    has_alias = true;
                    atok = toks[i + 3];
                }
                int existing_mod = -1;
                bool already_loaded = false;
                for (int u = 0; u < b->used_count; u++) {
                    if (strcmp(b->used_files[u], resolved) == 0) {
                        if (has_alias && b->used_mod_ids[u] > 0) {
                            existing_mod = b->used_mod_ids[u];
                            already_loaded = true;
                            break;
                        }
                        if (!has_alias && b->used_mod_ids[u] == 0) {
                            existing_mod = 0;
                            already_loaded = true;
                            break;
                        }
                    }
                }
                int target_mod = existing_mod;
                if (!already_loaded) {
                    target_mod = has_alias ? b->next_mod_id++ : 0;
                    bundle_file(b, resolved, target_mod);
                    if (b->failed) break;
                }
                if (has_alias && alias_count < B_MAX_ALIASES && atok.length < 64) {
                    memcpy(aliases[alias_count].alias, atok.start, (size_t)atok.length);
                    aliases[alias_count].alias[atok.length] = '\0';
                    aliases[alias_count].mod_id = target_mod;
                    alias_count++;
                }
            }
        }
    }

    if (b->failed) {
        free(toks);
        free(src);
        b->depth--;
        return;
    }

    /* Emit a clear module header comment before each inlined file. */
    if (b->out.len > 0) buf_str(&b->out, "\n");
    buf_str(&b->out, "// --- ");
    buf_str(&b->out, norm_path);
    buf_str(&b->out, " ---\n");

    /* Pass 2: copy source text, stripping `use` directives and rewriting
       namespaced references and declarations. */
    const char *cursor = src;
    brace = 0;
    typedef struct {
        char name[64];
        int depth;
    } LocalEntry;

    LocalEntry locals[B_MAX_LOCALS];
    int local_count = 0;
    bool in_fn_header = false;
    bool in_params = false;
    bool skip_test = false;
    int test_brace_start = 0;
    bool has_pending_for = false;
    char pending_for_name[64];
    int pending_for_len = 0;

    for (int i = 0; i < ntoks && toks[i].type != TOK_EOF; i++) {
        Token *tk = &toks[i];

        /* Strip `test "..." { ... }` blocks from imported modules so bundled
           files match `jlang run` behavior. */
        if (skip_test) {
            if (tk->type == TOK_LBRACE) brace++;
            else if (tk->type == TOK_RBRACE) {
                brace--;
                if (brace == test_brace_start) {
                    skip_test = false;
                    cursor = tk->start + tk->length;
                    if (*cursor == '\r') cursor++;
                    if (*cursor == '\n') cursor++;
                }
            }
            continue;
        }
        if (b->depth > 1 && brace == 0 && tk->type == TOK_TEST) {
            /* Emit anything before `test` up to start of line. */
            if (tk->start > cursor) buf_add(&b->out, cursor, (size_t)(tk->start - cursor));
            skip_test = true;
            test_brace_start = brace;
            continue;
        }

        /* Strip top-level `use "..." [as alias]` directives (already inlined). */
        if (brace == 0 && tk->type == TOK_USE && i + 1 < ntoks && toks[i + 1].type == TOK_STRING) {
            if (tk->start > cursor) buf_add(&b->out, cursor, (size_t)(tk->start - cursor));
            int end_i = i + 1;
            if (i + 3 < ntoks && tok_is_word(&toks[i + 2], "as") && toks[i + 3].type == TOK_IDENT) {
                end_i = i + 3;
            }
            cursor = toks[end_i].start + toks[end_i].length;
            if (*cursor == '\r') cursor++;
            if (*cursor == '\n') cursor++;
            i = end_i;
            continue;
        }

        /* Track function parameters, block-scoped `let` locals, and `for` loop
           variables with exact brace depths so shadowing ends cleanly at `}`. */
        bool is_for_var_decl = (i > 0 && toks[i - 1].type == TOK_FOR && tk->type == TOK_IDENT);
        if (brace == 0 && tk->type == TOK_FN) {
            local_count = 0;
            in_fn_header = true;
        } else if (in_fn_header && tk->type == TOK_LPAREN) {
            in_params = true;
        } else if (in_params && tk->type == TOK_RPAREN) {
            in_params = false;
        } else if (in_fn_header && tk->type == TOK_LBRACE) {
            in_fn_header = false;
        } else if (in_params && tk->type == TOK_IDENT) {
            if (local_count < B_MAX_LOCALS && tk->length > 0 && tk->length < 64) {
                memcpy(locals[local_count].name, tk->start, (size_t)tk->length);
                locals[local_count].name[tk->length] = '\0';
                locals[local_count].depth = 1;
                local_count++;
            }
        } else if (brace > 0 && tk->type == TOK_LET && i + 1 < ntoks && toks[i + 1].type == TOK_IDENT) {
            Token *nt = &toks[i + 1];
            if (local_count < B_MAX_LOCALS && nt->length > 0 && nt->length < 64) {
                memcpy(locals[local_count].name, nt->start, (size_t)nt->length);
                locals[local_count].name[nt->length] = '\0';
                locals[local_count].depth = brace;
                local_count++;
            }
        } else if (tk->type == TOK_FOR && i + 1 < ntoks && toks[i + 1].type == TOK_IDENT) {
            Token *nt = &toks[i + 1];
            if (nt->length > 0 && nt->length < 64) {
                memcpy(pending_for_name, nt->start, (size_t)nt->length);
                pending_for_name[nt->length] = '\0';
                pending_for_len = nt->length;
                has_pending_for = true;
            }
        }

        if (tk->type == TOK_LBRACE) {
            brace++;
            if (has_pending_for) {
                if (local_count < B_MAX_LOCALS) {
                    memcpy(locals[local_count].name, pending_for_name, (size_t)pending_for_len + 1);
                    locals[local_count].depth = brace;
                    local_count++;
                }
                has_pending_for = false;
            }
        } else if (tk->type == TOK_RBRACE) {
            if (brace > 0) brace--;
            while (local_count > 0 && locals[local_count - 1].depth > brace) {
                local_count--;
            }
        }

        bool prev_is_dot = (i > 0 && toks[i - 1].type == TOK_DOT);
        bool next_is_colon = (i + 1 < ntoks && toks[i + 1].type == TOK_COLON);
        bool is_local = is_for_var_decl;
        if (!is_local && (brace > 0 || in_params)) {
            for (int li = local_count - 1; li >= 0; li--) {
                if ((int)strlen(locals[li].name) == tk->length &&
                    memcmp(locals[li].name, tk->start, (size_t)tk->length) == 0) {
                    is_local = true;
                    break;
                }
            }
        }

        /* Rewrite `alias.member` -> `m<target>__member`. */
        if (tk->type == TOK_IDENT && !prev_is_dot && !is_local &&
            i + 2 < ntoks && toks[i + 1].type == TOK_DOT && toks[i + 2].type == TOK_IDENT) {
            int target_mod = find_local_alias(aliases, alias_count, tk->start, tk->length);
            if (target_mod > 0) {
                if (tk->start > cursor) buf_add(&b->out, cursor, (size_t)(tk->start - cursor));
                emit_prefix(&b->out, target_mod);
                buf_add(&b->out, toks[i + 2].start, (size_t)toks[i + 2].length);
                cursor = toks[i + 2].start + toks[i + 2].length;
                i += 2;
                continue;
            }
        }

        /* Inside a namespaced module, rewrite references & declarations of its
           own top-level symbols to `m<mod_id>__<name>`. */
        if (mod_id > 0 && tk->type == TOK_IDENT && !prev_is_dot && !next_is_colon && !is_local &&
            has_sym(top_syms, top_count, tk->start, tk->length)) {
            if (tk->start > cursor) buf_add(&b->out, cursor, (size_t)(tk->start - cursor));
            emit_prefix(&b->out, mod_id);
            buf_add(&b->out, tk->start, (size_t)tk->length);
            cursor = tk->start + tk->length;
            continue;
        }
    }

    /* Copy any remaining trailing comments/text. */
    size_t tail_len = strlen(cursor);
    if (tail_len > 0) {
        buf_add(&b->out, cursor, tail_len);
        if (cursor[tail_len - 1] != '\n') buf_str(&b->out, "\n");
    }

    free(toks);
    free(src);
    b->depth--;
}

char *jl_bundle(const char *entry_path, char *errbuf, int errbuf_size) {
    char norm[512];
    if (!normalize_path(entry_path, norm, sizeof norm)) {
        if (errbuf && errbuf_size > 0) snprintf(errbuf, (size_t)errbuf_size, "file path is too long");
        return NULL;
    }

    Bundler b;
    memset(&b, 0, sizeof b);
    b.next_mod_id = 1;
    b.errbuf = errbuf;
    b.errsize = errbuf_size;

    bundle_file(&b, norm, 0);

    for (int i = 0; i < b.used_count; i++) free(b.used_files[i]);
    if (b.failed || b.out.oom) {
        free(b.out.data);
        if (!b.failed && errbuf && errbuf_size > 0) snprintf(errbuf, (size_t)errbuf_size, "out of memory");
        return NULL;
    }
    return b.out.data ? b.out.data : calloc(1, 1);
}
