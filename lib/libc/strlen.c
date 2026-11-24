#include "libc.h"

size_t
strlen(const char *s)
{
    size_t len;

    len = 0;
    while (s[len] != '\0') {
        ++len;
    }
    return len;
}
