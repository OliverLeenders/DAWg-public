/* LAME's configuration for DAWg's build (cmake/Dependencies.cmake), in place of what its configure script writes.
   The same on every platform: plain C, no assembly or SSE paths, no decoder. */
#ifndef DAWG_LAME_CONFIG_H
#define DAWG_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define PACKAGE "lame"

#include <stdint.h>

typedef long double ieee854_float80_t;
typedef double ieee754_float64_t;
typedef float ieee754_float32_t;

#endif
