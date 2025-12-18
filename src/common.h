#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define NES_LIKELY(x)   __builtin_expect(!!(x), 1)
#define NES_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NES_LIKELY(x)   (x)
#define NES_UNLIKELY(x) (x)
#endif

