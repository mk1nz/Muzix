#include "libc.h"

char *
strrchr(const char *s, int c)
{
    char *last;

    last = 0;
    while (*s) {
        if (*s == (char)c) {
            last = (char *)s;
        }
        ++s;
    }
    if (c == '\0') {
        return (char *)s;
    }
    return last;
}
