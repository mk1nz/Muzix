#include "libc.h"

char *
strcpy(char *dest, const char *src)
{
    char *d;

    d = dest;
    while ((*d++ = *src++) != '\0') {}
    return dest;
}
