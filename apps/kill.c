#include "../lib/libc.h"
#include "../lib/syscall.h"

static int
parse_num(const char *s, int *out)
{
    int value = 0;

    if (*s == '\0') {
        return -1;
    }
    while (*s != '\0') {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        value = value * 10 + (*s - '0');
        s++;
    }
    *out = value;
    return 0;
}

int
main(int argc, char *argv[])
{
    int pid;
    int signal = 15;
    int i;

    if (argc < 2) {
        puts("usage: kill [-signal] pid");
        return 1;
    }

    i = 1;
    if (argv[i][0] == '-' && argv[i][1] != '\0') {
        if (parse_num(argv[i] + 1, &signal) != 0 ||
            signal < 1 || signal > 16) {
            puts("kill: bad signal number");
            return 1;
        }
        i++;
    }

    if (i >= argc) {
        puts("usage: kill [-signal] pid");
        return 1;
    }

    if (parse_num(argv[i], &pid) != 0 || pid < 1 || pid > 255) {
        puts("kill: bad pid");
        return 1;
    }

    if (sys_signal(pid, signal) != 0) {
        puts("kill: signal not delivered");
        return 1;
    }

    return 0;
}
