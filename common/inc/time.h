#ifndef CARDINAL_STD_TIME_H
#define CARDINAL_STD_TIME_H

#include <stddef.h>

typedef long clock_t;
typedef long time_t;

struct timespec {
  time_t tv_sec;
  long tv_nsec;
};

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

struct tm *gmtime(const time_t *);

size_t strftime(char *restrict __s, size_t __maxsize,
                const char *restrict __format, const struct tm *restrict __tp);

#endif