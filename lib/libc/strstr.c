#include "libc.h"

char *
strstr(const char *haystack, const char *needle)
{
    const char *h;
    const char *n;

    if (*needle == '\0') {
        return (char *)haystack;
    }

    for (h = haystack; *h != '\0'; ++h) {
        if (*h == *needle) {
            for (n = needle + 1; *n != '\0'; ++n) {
                if (h[(n - needle)] != *n) {
                    break;
                }
            }
            if (*n == '\0') {
                return (char *)h;
            }
        }
    }
    return 0;
}
