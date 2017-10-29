#include <time.h>
#include <types.h>

struct tm *WEAK gmtime(const time_t *a) {
  static struct tm t;

  t.tm_sec = *a % 60;
  t.tm_min = *a / 60 % 60;
  t.tm_hour = *a / (60 * 60) % 24;
  // TODO: finish implementation

  return &t;
}

size_t WEAK strftime(char *restrict __s, size_t __maxsize,
                     const char *restrict __format,
                     const struct tm *restrict __tp) {
  *__s = 'a';
  *(__s + 1) = 0;
  __maxsize = 0;
  __format = NULL;
  __tp = NULL;

  return 1;
}