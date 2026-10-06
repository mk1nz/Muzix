#include <stddef.h>

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *c1 = (const unsigned char *)s1;
    const unsigned char *c2 = (const unsigned char *)s2;
    while (n--) {
        if (*c1 != *c2) {
            return *c1 - *c2;
        }
        c1++;
        c2++;
    }
    return 0;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *ptr = dest;
    while (*ptr) {
        ptr++;
    }
    while (*src) {
        *ptr++ = *src++;
    }
    *ptr = '\0';
    return dest;
}

char *strrchr(const char *s, int c)
{
    char *found = NULL;
    while (*s) {
        if (*s == (char)c) {
            found = (char *)s;
        }
        s++;
    }
    if (c == '\0') {
        found = (char *)s;
    }
    return found;
}
