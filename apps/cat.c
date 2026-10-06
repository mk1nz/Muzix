/*
 * cat - concatenate and print files
 */

#include "../lib/libc.h"

int main(int argc, char *argv[])
{
    int failed = 0;

    if (argc < 2) {
        puts("Usage: cat <file> [file ...]");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) {
            puts("cat: cannot open ");
            puts(argv[i]);
            failed = 1;
            continue;
        }
        
        char buffer[512];
        int n;
        while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
            if (write(1, buffer, n) != n) {
                failed = 1;
                break;
            }
        }
        if (n < 0) {
            failed = 1;
        }
        
        close(fd);
    }
    
    return failed ? 1 : 0;
}
