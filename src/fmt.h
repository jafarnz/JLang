/*
 * fmt.h - zero-config source code formatter for JLang.
 */
#ifndef JL_FMT_H
#define JL_FMT_H

/* Formats JLang source text with standard 4-space indentation and spacing.
 * Returns a newly allocated string the caller must free(), or NULL on a
 * lexing error (with a message written to errbuf if non-NULL). */
char *jl_format(const char *src, char *errbuf, int errbuf_size);

#endif
