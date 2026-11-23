/*
 * port.h - the few things the jlang command needs from the operating
 * system, so it builds on Linux, macOS and Windows alike. The language
 * core (compiler, VM, heap) uses none of them.
 */
#ifndef JL_PORT_H
#define JL_PORT_H

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <string.h>
#include <windows.h>
#define jl_isatty_stdout() _isatty(1)
#define jl_mkdir(path) _mkdir(path)
#define jl_sleep_ms(ms) Sleep((DWORD)(ms))
#define jl_strncasecmp _strnicmp
#else
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define jl_isatty_stdout() isatty(STDOUT_FILENO)
#define jl_mkdir(path) mkdir(path, 0755)
static inline void jl_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#define jl_strncasecmp strncasecmp
#endif

#endif
