/*
 * svd.h - turn a chip's SVD register description into JLang consts.
 */
#ifndef JL_SVD_H
#define JL_SVD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Writes consts for the registers and fields in `xml` to `out`. `only`
 * lists peripherals ("GPIO") or registers ("GPIO.OUT") to keep; with
 * nonly == 0 everything is written. On failure, err says why. */
bool svd_to_jlang(const char *xml, const char *source_name, char **only, int nonly, FILE *out, char *err,
                  size_t errlen);

#endif
