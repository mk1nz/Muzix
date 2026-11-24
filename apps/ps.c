#include "../lib/libc.h"
#include "../lib/syscall.h"

/* Print a decimal, right-aligned in `width` columns. */
static void
put_field(uint32_t value, int width)
{
    static const uint32_t place[10] = {
        1000000000u, 100000000u, 10000000u, 1000000u, 100000u,
        10000u, 1000u, 100u, 10u, 1u
    };
    char digits[10];
    int length = 0;
    int pad;
    int i = 0;

    while (i < 9 && value < place[i]) {
        i++;
    }
    for (; i < 10; i++) {
        int digit = 0;

        while (value >= place[i]) {
            value -= place[i];
            digit++;
        }
        digits[length++] = (char)('0' + digit);
    }

    for (pad = length; pad < width; pad++) {
        putchar(' ');
    }
    for (i = 0; i < length; i++) {
        putchar(digits[i]);
    }
}

static char
state_of(const struct proc_info *info)
{
    if (!info->active) {
        return '-';
    }
    if (info->pid == 0) {
        return 'K';
    }
    if (info->exited) {
        return 'Z';
    }
    return 'R';
}

static char
queue_of(const struct proc_info *info)
{
    switch (info->ready_q) {
    case MUZIX_PROC_Q_TASK:
        return 'T';
    case MUZIX_PROC_Q_SERVER:
        return 'S';
    default:
        return 'U';
    }
}

static void
put_str(const char *s)
{
    while (*s) {
        putchar(*s++);
    }
}

static void
put_command(const struct proc_info *info)
{
    const char *s = info->name;
    int i = 0;

    if (!info->active) {
        put_str("[free]");
        return;
    }
    if (info->pid == 0) {
        put_str("[kernel]");
        return;
    }
    while (i < MUZIX_PROC_NAME_SIZE - 1 && s[i] != '\0') {
        i++;
    }
    if (i == 0) {
        putchar('?');
        return;
    }
    while (i > 0 && s[i - 1] != '/') {
        i--;
    }
    if (s[i] == '\0') {
        putchar('?');
        return;
    }
    while (i < MUZIX_PROC_NAME_SIZE - 1 && s[i] != '\0') {
        putchar(s[i++]);
    }
}

int
main(int argc, char *argv[])
{
    struct proc_info info[MUZIX_PROC_INFO_SLOTS];
    int slot;

    (void)argc;
    (void)argv;

    put_str("  PID  PPID Q ST     UTIME      STIME     CUTIME     CSTIME  COMMAND");
    putchar('\n');

    for (slot = 0; slot < MUZIX_PROC_INFO_SLOTS; slot++) {
        if (sys_proctab(&info[slot], slot) != 0) {
            info[slot].active = 0;
            info[slot].pid = 0;
            info[slot].ppid = 0;
            info[slot].ready_q = MUZIX_PROC_Q_USER;
            info[slot].exited = 0;
            info[slot].name[0] = '\0';
        }
        if (!info[slot].active) {
            continue;
        }

        put_field(info[slot].pid, 5);
        put_field(info[slot].ppid, 6);
        putchar(' ');
        putchar(queue_of(&info[slot]));
        putchar(' ');
        putchar(state_of(&info[slot]));
        put_field(info[slot].user_time, 10);
        put_field(info[slot].sys_time, 10);
        put_field(info[slot].child_user_time, 10);
        put_field(info[slot].child_sys_time, 10);
        putchar(' ');
        putchar(' ');
        put_command(&info[slot]);
        putchar('\n');
    }

    return 0;
}
