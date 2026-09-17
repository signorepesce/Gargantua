#ifndef GARGANTUA_H
#define GARGANTUA_H

#include "version.h"
#include "detail/runtime.h"

#define $format(...) str_format(__VA_ARGS__)
#define $concat(a, b) str_concat((a), (b))
#define $pick(...) GARGANTUA_ERROR_pick_REQUIRES_GENERATOR

#endif
