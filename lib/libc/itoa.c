#include "libc.h"

char *
itoa(int value, char *str, int base)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char buffer[33];
    char *ptr;
    int negative;

    ptr = buffer + sizeof(buffer) - 1;
    negative = 0;

    if (base < 2 || base > 36) {
        return NULL;
    }

    *ptr = '\0';

    if (value < 0) {
        negative = 1;
        value = -value;
    }

    do {
        int rest;
        int quo;

        rest = value;
        quo = 0;

        while (rest >= base) {
            rest = rest - base;
            ++quo;
        }
        *--ptr = digits[rest];
        value = quo;
    } while (value);

    if (negative) {
        *--ptr = '-';
    }

    strcpy(str, ptr);
    return str;
}
