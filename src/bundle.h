/*
 * bundle.h - single-file bundler for multi-module JLang projects.
 *
 * Resolves all `use "path.jlang"` and `use "path.jlang" as alias` imports
 * into one self-contained .jlang file that runs identically on the desktop
 * CLI, browser WebAssembly playground, and ESP32 VM firmware.
 */
#ifndef JL_BUNDLE_H
#define JL_BUNDLE_H

/* Bundles entry_path and all of its transitive `use` imports into a newly
 * allocated NUL-terminated string (caller must free()), or returns NULL on
 * error (writing a diagnostic into errbuf). */
char *jl_bundle(const char *entry_path, char *errbuf, int errbuf_size);

#endif
