#ifndef _STDDEF_H_
#define _STDDEF_H_

#define NULL ((void *)0)

#define offsetof(type, member) ((size_t)(&((type *)0)->member))

#endif