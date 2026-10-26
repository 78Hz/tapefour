/* LAME's build configuration for TAPE-FOUR, in place of the one autotools would write.
   The same on Windows (MSVC), macOS and Linux: standard C99 headers, IEEE floats. */
#ifndef TAPEFOUR_LAME_CONFIG_H
#define TAPEFOUR_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1

/* IEEE 754 floats everywhere TAPE-FOUR runs: the faster code paths are safe. */
typedef float ieee754_float32_t;
#define TAKEHIRO_IEEE754_HACK 1
#define USE_FAST_LOG 1

/* Encoding only: no decoder, no assembly. */
#undef HAVE_MPGLIB
#undef DECODE_ON_THE_FLY
#undef HAVE_NASM

#define PACKAGE "lame"
#define VERSION "3.99.5"

#if defined (_MSC_VER)
 #define _CRT_SECURE_NO_WARNINGS 1
 #pragma warning (disable : 4244 4305 4267 4146 4018 4996)
#endif

#endif
