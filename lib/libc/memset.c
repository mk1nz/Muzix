#include "libc.h"

void *
memset(void *s, int c, size_t n)
{
    unsigned char *p;

    p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}
