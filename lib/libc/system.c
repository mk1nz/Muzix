#include "libc.h"
#include "syscall.h"

int
system(const char *command)
{
    int pid;
    int status;

    pid = fork();
    if (pid == 0) {
        const char *argv[] = { "shell", "-c", (char *)command, NULL };
        exec("/bin/shell", argv);
        exit(1);
    } else if (pid > 0) {
        status = 0;
        wait(&status);
        return status;
    }
    return -1;
}
