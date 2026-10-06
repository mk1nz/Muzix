/*
 * ls - list directory contents
 *
 * Three flags, and the reason there are three rather than seven is the
 * filesystem rather than the taste.
 *
 * The image uses the MINIX-v1 filesystem format, is built by
 * tools/make_minixfs.rb, and is read-only and flat. So:
 *
 *   -a  would be nothing.  There are no hidden entries: the directory holds no
 *       name beginning with a dot except "." and "..", which are always listed.
 *   -R  would be nothing.  There are no subdirectories; the whole tree is the
 *       root, which is what `ls` prints.
 *   -t  and -l's usual date column would be nothing.  muzix_fs_inode_t has no
 *       timestamp field at all (fs/fs_types.h), so there is nothing to print
 *       and printing a zero or a dash would be a fiction.
 *   -F  would mark executables, and there is no executable bit: mode carries
 *       only a type, and make_minixfs.rb writes one mode for the root and none
 *       for the files.
 *
 * What IS on this filesystem is a size, and it is worth having: knowing how big
 * each program in the image is, and that the sum is what you expect, is the
 * difference between "I flashed something" and "I flashed what I meant to".
 *
 *   -l   long format: mode, link count, size, name
 *   -1   names only, without "." and ".." - for reading into a shell variable
 *   -r   reverse order
 *
 * With no flags the output is exactly what it has always been, because that is
 * what everything written against this command already expects.
 */

#include "../lib/libc.h"
#include "../lib/syscall.h"   /* sys_stat: the inode layout comes back through it */

/* The inode layout, from the kernel's own header rather than copied here.
 *
 * sys_stat() copies a muzix_fs_inode_t into the caller's buffer and userspace
 * has no declaration for it, so the alternative was a second, hand-written copy
 * of the struct in this file - two definitions of the same layout, one of which
 * the compiler cannot check against the other.  Including fs/fs_types.h is the
 * coupling made explicit instead.  It is safe to include from userspace: it
 * wants nothing but <stdint.h>, which is already here, and it is types and
 * constants rather than anything that reaches the memory manager.
 */
#include "../fs/fs_types.h"

#define LS_PATH_MAX 64

/* Right-align an unsigned number in `width` columns.
 *
 * No division: 32-bit `/` is __divulong, which is not in any userspace link
 * here, and an unresolved global on this machine becomes a call into the boot
 * stub.  So the place values are a table and each digit comes out by repeated
 * subtraction, which is what apps/top.c does for the same reason and with the
 * same reasoning.  If a third program needs this, it belongs in lib/libc.c.
 *
 * Ten digits is the most a uint32_t can need, so digits[10] is exact. */
static void put_size(uint32_t value, int width)
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
    /* The loop above filled digits[] most significant first, so it is printed
     * in that order.  Printing it from the end - which is the obvious thing,
     * because that is the order the digits come out of the extraction - reverses
     * the number: 4526 comes out as 6254, 692 as 296, 1488 as 8841.  Every size
     * this program prints was wrong by exactly that reversal, and stat() was
     * never at fault. */
    for (i = 0; i < length; i++) {
        putchar(digits[i]);
    }
}

/* One letter of the type, from the mode's type nibble.
 *
 * MINIX keeps the type in the top four bits, which is why fs/fs_types.h has
 * MUZIX_FS_MODE_TYPE and friends named already.  A file whose mode says
 * nothing about its type falls through to '-', which is the honest answer for
 * a filesystem where the image writer never set one. */
static char type_of(uint16_t mode)
{
    switch (mode & MUZIX_FS_MODE_TYPE) {
        case MUZIX_FS_MODE_DIR:
            return 'd';
        case MUZIX_FS_MODE_BLOCK:
            return 'b';
        case MUZIX_FS_MODE_CHAR:
            return 'c';
        default:
            return '-';
    }
}

/* The mode as ten characters: the type letter and nine permission bits.
 *
 * MINIX keeps the type in the top nibble and owner/group/other permissions
 * below it, which is why this reads bits off a shift rather than masking a
 * printed constant - fs/fs_types.h names the type bits but not a per-group one.
 *
 * Worth saying plainly, because a column of -rwxrwxrwx down the page invites the
 * wrong conclusion: this is what the image stores, not what will stop anyone.
 * tools/make_minixfs.rb writes 0x8000 | 0777 for every file it packs, so every
 * program and every document here reads -rwxrwxrwx, and "." and ".." read
 * d--------- because the root inode's mode carries no permissions at all - which
 * in MINIX means no access, not "unrestricted".
 *
 * The bits are real, not decoration.  muzix_fs_service_check_inode_access()
 * shifts owner, group and other by 6, 3 and 0, refuses when the mode is 0000,
 * and gives uid 0 an exception for exec only; SYS_CHMOD and SYS_ACCESS are both
 * dispatched to implemented handlers; and open() goes through it.  An image
 * where everything is 0777 is one nobody chose the modes of. */
static void put_mode(uint16_t mode)
{
    /* The MINIX permission table, indexed by the three
     * permission bits with +8 for the special one, which is where the 's' in
     * place of the 'x' comes from.  Reproduced rather than recomputed: the
     * indexing is the whole mechanism, and a shift-and-test of one's own would
     * be a second opinion about the same table.
     *
     * The bit within a group runs the other way from the letter it prints: bit 0
     * is execute, so this is indexed by bit and not by position. */
    static const char *const rwx[16] = {
        "---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx",
        "--s", "--s", "-ws", "-ws", "r-s", "r-s", "rws", "rws"
    };
    uint16_t m = mode & MUZIX_FS_MODE_PERMS;
    int group;
    int index;

    putchar(type_of(mode));
    for (group = 0; group < 3; group++) {
        /* A dash between the groups, which MINIX does not print and everyone
         * now expects: -rw-r--r-- rather than rw-r--r--.  One column wider on a
         * 150-wide console, and the group boundaries stop having to be counted.
         * This is the GNU and BSD convention and a deliberate departure from
         * the reference, which prints "%c%s%s%s". */
        if (group != 0) {
            putchar('-');
        }
        index = (m >> (6 - group * 3)) & 7;
        if (group == 0 && (m & MUZIX_FS_MODE_SETUID) != 0) {
            index += 8;
        }
        if (group == 1 && (m & MUZIX_FS_MODE_SETGID) != 0) {
            index += 8;
        }
        putchar(rwx[index][0]);
        putchar(rwx[index][1]);
        putchar(rwx[index][2]);
    }
}

/* "." and "..", and nothing else that starts with a dot - there is nothing
 * else, but the test is written for the general case so it does not start
 * lying if the image ever gains a real hidden file. */
static int is_dot(const char *name)
{
    if (name[0] != '.') {
        return 0;
    }
    if (name[1] == 0) {
        return 1;
    }
    return name[1] == '.' && name[2] == 0;
}

static void usage(void)
{
    puts("usage: ls [-l] [-1] [-r] [directory]");
    puts("");
    puts("  -l   long format: mode, link count, size, name");
    puts("  -1   names only, without \".\" and \"..\"");
    puts("  -r   reverse order");
}

/* Print one entry.  The size needs a stat() on the entry's own path, which is
 * why this is a function rather than three lines in the loop: the loop has two
 * orders in it and both of them want the same three lines. */
static void show(const char *dir, const struct dirent *entry, int long_fmt)
{
    muzix_fs_inode_t info;
    char full[LS_PATH_MAX];
    size_t used = 0;
    size_t i;
    int known;

    if (!long_fmt) {
        puts(entry->name);
        return;
    }

    /* Build "<dir>/<name>", bounded.  A name is at most 14 characters and the
     * path came from the command line, so this fits; the check is here so that a
     * long argument produces a short answer rather than a smashed stack. */
    for (i = 0; dir[i] != 0 && used < LS_PATH_MAX - 1; i++) {
        full[used++] = dir[i];
    }
    if (used < LS_PATH_MAX - 1 && (used == 0 || dir[i - 1] != '/')) {
        full[used++] = '/';
    }
    for (i = 0; entry->name[i] != 0 && used < LS_PATH_MAX - 1; i++) {
        full[used++] = entry->name[i];
    }
    full[used] = 0;

    /* "." and ".." are not reachable by path on a flat root - there is no parent
     * to name - so a stat() that fails is not an error here, it is an entry whose
     * mode this filesystem cannot answer.  '?' says that; a zero would not. */
    known = (sys_stat(full, &info) == 0);

    /* MINIX's own order: mode, link count, size, name. */
    put_mode(known ? info.mode : 0);
    putchar(' ');
    if (known) {
        put_size(info.links, 2);
    } else {
        puts("  ?");
    }
    putchar(' ');
    if (known) {
        put_size(info.size, 7);
    } else {
        puts("      ?");
    }
    putchar(' ');
    puts(entry->name);
}

int main(int argc, char *argv[])
{
    const char *path = ".";
    int long_fmt = 0;
    int ones_only = 0;
    int reverse = 0;
    int fd;
    int result;
    int index = 0;
    int total = 0;
    int argi;

    for (argi = 1; argi < argc; argi++) {
        const char *arg = argv[argi];

        if (arg[0] != '-' || arg[1] == 0) {
            path = arg;
            break;
        }
        if (arg[1] == '-' && arg[2] == 0) {
            /* "--" ends the flags, so a directory whose name begins with a dash
             * can still be listed. */
            argi++;
            if (argi < argc) {
                path = argv[argi];
            }
            break;
        }
        {
            const char *flag;

            for (flag = arg + 1; *flag != 0; flag++) {
                switch (*flag) {
                    case 'l':
                        long_fmt = 1;
                        break;
                    case '1':
                        ones_only = 1;
                        break;
                    case 'r':
                        reverse = 1;
                        break;
                    default:
                        puts("ls: unknown option");
                        usage();
                        return 1;
                }
            }
        }
    }

    fd = open(path, 0);
    if (fd < 0) {
        puts("ls: cannot open directory");
        return 1;
    }

    /* Reverse order without a buffer.
     *
     * readdir() here is addressed by index rather than being a sequential
     * iterator, which is what makes this cheap: one pass to find how many
     * entries there are, then the same pass backwards.  Sorting by name would
     * need somewhere to put the names - a directory of a hundred entries is 1.6
     * KB of names in a 16 KB text window - and with twelve entries in the root
     * it would buy nothing. */
    if (reverse) {
        struct dirent entry;

        result = readdir(fd, index++, &entry);
        while (result == 0) {
            total++;
            result = readdir(fd, index++, &entry);
        }
    }

    index = 0;
    for (;;) {
        struct dirent entry;
        int at = reverse ? (total - 1 - index) : index;

        result = readdir(fd, at, &entry);
        if (result != 0) {
            break;
        }
        if (ones_only && is_dot(entry.name)) {
            index++;
            continue;
        }
        show(path, &entry, long_fmt);
        index++;
    }

    close(fd);
    return 0;
}
