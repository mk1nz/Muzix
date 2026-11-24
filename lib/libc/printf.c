#include "libc.h"

/* Print an unsigned value in the given base.
 *
 * Counts digits down through a table of place values, taking each digit by
 * repeated subtraction rather than `/` and `%`.  Same rationale as in the
 * comment above: the division helpers had register-return disagreements on
 * this target, and repeated subtraction is provably correct.
 */
static void
print_unsigned(unsigned int value, unsigned int base, unsigned int width, int zeropad)
{
    static const char digits[] = "0123456789abcdef";
    static const unsigned int places_dec[] = { 10000, 1000, 100, 10, 1 };
    static const unsigned int places_hex[] = { 4096, 256, 16, 1 };
    const unsigned int *places;
    unsigned int nplaces;
    char buffer[8];
    char *out;
    unsigned int start;
    unsigned int i;
    unsigned int ndigits;

    places = (base == 16) ? places_hex : places_dec;
    nplaces = (base == 16) ? 4u : 5u;
    out = buffer;

    for (start = 0; start + 1u < nplaces && value < places[start]; ++start) {
        continue;
    }

    for (i = start; i < nplaces; ++i) {
        unsigned int digit;

        digit = 0;
        while (value >= places[i]) {
            value = (unsigned int)(value - places[i]);
            ++digit;
        }
        *out++ = digits[digit];
    }
    *out = '\0';

    ndigits = (unsigned int)(out - buffer);
    if (zeropad && width > ndigits) {
        for (i = 0; i < width - ndigits; ++i) {
            putchar('0');
        }
    }

    for (out = buffer; *out; ++out) {
        putchar(*out);
    }
}

void
printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            putchar(*fmt++);
            continue;
        }
        ++fmt;
        {
            int zeropad;
            unsigned int width;

            zeropad = 0;
            width = 0;
            while (*fmt == '0' || (*fmt >= '1' && *fmt <= '9')) {
                if (*fmt == '0') {
                    zeropad = 1;
                } else {
                    width = width * 10 + (unsigned int)(*fmt - '0');
                }
                ++fmt;
            }
            switch (*fmt++) {
            case '%':
                putchar('%');
                break;
            case 'c':
                putchar(va_arg(args, int));
                break;
            case 's': {
                const char *value;

                value = va_arg(args, const char *);
                while (*value) {
                    putchar(*value++);
                }
                break;
            }
            case 'd': {
                int value;

                value = va_arg(args, int);
                if (value < 0) {
                    putchar('-');
                    value = -value;
                }
                print_unsigned((unsigned int)value, 10, width, zeropad);
                break;
            }
            case 'u':
                print_unsigned(va_arg(args, unsigned int), 10, width, zeropad);
                break;
            case 'x':
                print_unsigned(va_arg(args, unsigned int), 16, width, zeropad);
                break;
            default:
                putchar('?');
                break;
            }
        }
    }
    va_end(args);
}
