#include "libc.h"

int
atoi(const char *nptr)
{
    int result;
    int sign;

    result = 0;
    sign = 1;

    while (isspace(*nptr)) {
        ++nptr;
    }

    if (*nptr == '-') {
        sign = -1;
        ++nptr;
    } else if (*nptr == '+') {
        ++nptr;
    }

    while (isdigit(*nptr)) {
        result = result * 10 + (*nptr - '0');
        ++nptr;
    }

    if (sign < 0) {
        result = -result;
    }
    return result;
}
