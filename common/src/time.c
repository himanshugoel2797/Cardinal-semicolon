#include <time.h>
#include <types.h>

// Days in each month for a non-leap year.
static const int k_month_days[12] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};

static const char *const k_wday_abbr[7] = {"Sun", "Mon", "Tue", "Wed",
                                          "Thu", "Fri", "Sat"};
static const char *const k_mon_abbr[12] = {"Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec"};

static int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// Convert a UTC time_t (seconds since the 1970-01-01 epoch) into a broken-down
// struct tm. Designed for non-negative (post-epoch) times, the only case the
// kernel produces; negative inputs are normalised but the pre-1970 calendar
// walk is best-effort.
struct tm *WEAK gmtime(const time_t *a) {
    static struct tm t;

    long secs = (long)*a;
    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) {  // normalise so 0 <= rem < 86400
        rem += 86400;
        days--;
    }

    t.tm_hour = (int)(rem / 3600);
    t.tm_min = (int)((rem % 3600) / 60);
    t.tm_sec = (int)(rem % 60);

    // 1970-01-01 was a Thursday (tm_wday == 4).
    t.tm_wday = (int)(((days % 7) + 4) % 7);
    if (t.tm_wday < 0)
        t.tm_wday += 7;

    int year = 1970;
    while (days >= (is_leap(year) ? 366 : 365)) {
        days -= is_leap(year) ? 366 : 365;
        year++;
    }
    t.tm_year = year - 1900;
    t.tm_yday = (int)days;

    int mon = 0;
    while (mon < 12) {
        int dm = k_month_days[mon] + ((mon == 1 && is_leap(year)) ? 1 : 0);
        if (days < dm)
            break;
        days -= dm;
        mon++;
    }
    t.tm_mon = mon;
    t.tm_mday = (int)days + 1;
    t.tm_isdst = 0;

    return &t;
}

// Append a string to the output, respecting the buffer limit. Returns false if
// it does not fit (the caller then bails, matching strftime's "return 0").
static int append_str(char *s, size_t maxsize, size_t *pos, const char *str) {
    while (*str) {
        if (*pos + 1 >= maxsize)  // leave room for the terminating NUL
            return 0;
        s[(*pos)++] = *str++;
    }
    return 1;
}

// Append `val` as a `width`-digit zero-padded decimal (width 0 = natural width).
static int append_num(char *s, size_t maxsize, size_t *pos, int val, int width) {
    char buf[16];
    int n = 0;
    unsigned int v = (val < 0) ? (unsigned int)(-val) : (unsigned int)val;

    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);

    while (n < width)
        buf[n++] = '0';
    if (val < 0)
        buf[n++] = '-';

    while (n > 0) {
        if (*pos + 1 >= maxsize)
            return 0;
        s[(*pos)++] = buf[--n];
    }
    return 1;
}

// A practical subset of strftime. Supports: %Y %y %m %d %H %M %S %j %p %e
// %a %b %% (and treats an unknown %x as a literal '%' + the char). Returns the
// number of characters written (excluding the NUL), or 0 if the result did not
// fit in __maxsize (in which case the buffer contents are unspecified, per spec).
size_t WEAK strftime(char *restrict __s, size_t __maxsize,
                     const char *restrict __format,
                     const struct tm *restrict __tp) {
    if (__maxsize == 0)
        return 0;

    size_t pos = 0;
    const char *f = __format;
    int ok = 1;

    while (*f && ok) {
        if (*f != '%') {
            if (pos + 1 >= __maxsize) {
                ok = 0;
                break;
            }
            __s[pos++] = *f++;
            continue;
        }

        f++;  // consume '%'
        switch (*f) {
            case 'Y': ok = append_num(__s, __maxsize, &pos, __tp->tm_year + 1900, 0); break;
            case 'y': ok = append_num(__s, __maxsize, &pos, (__tp->tm_year + 1900) % 100, 2); break;
            case 'm': ok = append_num(__s, __maxsize, &pos, __tp->tm_mon + 1, 2); break;
            case 'd': ok = append_num(__s, __maxsize, &pos, __tp->tm_mday, 2); break;
            case 'e': ok = append_num(__s, __maxsize, &pos, __tp->tm_mday, 0); break;
            case 'H': ok = append_num(__s, __maxsize, &pos, __tp->tm_hour, 2); break;
            case 'M': ok = append_num(__s, __maxsize, &pos, __tp->tm_min, 2); break;
            case 'S': ok = append_num(__s, __maxsize, &pos, __tp->tm_sec, 2); break;
            case 'j': ok = append_num(__s, __maxsize, &pos, __tp->tm_yday + 1, 3); break;
            case 'p': ok = append_str(__s, __maxsize, &pos, __tp->tm_hour < 12 ? "AM" : "PM"); break;
            case 'a':
                if (__tp->tm_wday >= 0 && __tp->tm_wday < 7)
                    ok = append_str(__s, __maxsize, &pos, k_wday_abbr[__tp->tm_wday]);
                break;
            case 'b':
                if (__tp->tm_mon >= 0 && __tp->tm_mon < 12)
                    ok = append_str(__s, __maxsize, &pos, k_mon_abbr[__tp->tm_mon]);
                break;
            case '%': ok = append_str(__s, __maxsize, &pos, "%"); break;
            case '\0': ok = append_str(__s, __maxsize, &pos, "%"); continue;  // trailing '%'
            default:
                // Unknown specifier: emit it verbatim ("%x").
                if (pos + 2 >= __maxsize) {
                    ok = 0;
                } else {
                    __s[pos++] = '%';
                    __s[pos++] = *f;
                }
                break;
        }
        if (*f)
            f++;
    }

    if (!ok)
        return 0;

    __s[pos] = '\0';
    return pos;
}
