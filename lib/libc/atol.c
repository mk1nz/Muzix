#include "libc.h"

long
atol(const char *nptr)
{
    return (long)atoi(nptr);
}
