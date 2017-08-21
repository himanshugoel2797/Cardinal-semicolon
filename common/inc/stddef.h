#ifndef _STDDEF_H_
#define _STDDEF_H_

#define NEED_SIZE_T_ONLY 1
#undef CARDINAL_TYPES_H
#include "types.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) ((size_t)(&((type *)0)->member))

#endif