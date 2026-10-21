/*
 * Muzix Shell - command-line interpreter for Muzix on Z80
 * 
 * Features:
 * - Built-in commands (cd, pwd, echo, exit, help)
 * - External command execution via fork/exec
 * - Process waiting and status handling
 * - Working directory tracking
 */

#include "../lib/libc.h"
#include "../lib/syscall.h"
#include "../include/muzix_ipc.h"
#include "muzix_build_stamp.h"

/* The FS server's PROCESS NUMBER, from include/muzix_ipc.h - the one place the
 * spelling exists.
 *
 * This used to be a locally-invented 11, on the theory that an IPC slot is
 * `10 + server_id`.  That 10 is an ARRAY OFFSET inside procs[], not part of the
 * identity.  Passed as a destination it selected procs[21], a slot nobody
 * registered, and every call in this file was answered with EFAULT.
 *
 * Do NOT reintroduce a local number, and do NOT recompute one from an id or a
 * slot.  If a new server is needed, add a MUZIX_PROC_* to the header. */
#define FS_SERVER_IPC_SLOT MUZIX_PROC_FS

/* IPC-based FS operations */
static int ipc_chdir(const char *path)
{
    return sys_chdir(path);
}

/* Returns what sys_getcwd returns - a pointer to the buffer, or NULL on
 * failure.  This used to be `int`, and libc's sys_getcwd() returns char *
 * (lib/syscall.h), so every return smuggled a pointer through an int and
 * refresh_cwd()'s `< 0` test was reading a pointer as a status. */
static char *ipc_getcwd(char *buf, int size)
{
    /* Same raw-sendrec defect the old read path had: the non-parking else
     * returned the request's own m1_i1 (0) and the reply was copied later into
     * the dead frame's &msg, so the prompt's cwd never arrived and the stale
     * copy could land on a live stack frame.  sys_getcwd() is the routed
     * SYS_GETCWD: it parks the caller exactly like sys_read()/sys_write() and
     * the FS's handle_getcwd() copies the string through the same
     * deliver_parked path. */
    return sys_getcwd(buf, size);
}

/* Which tree this binary was built from, written by the Makefile.  The fallback
 * is for compiling shell.c outside make, which the host tests do. */
#ifndef MUZIX_BUILD_ID
#define MUZIX_BUILD_ID "unknown"
#endif

#define MAX_COMMAND 256
#define MAX_ARGS 16
#define MAX_PATH 128

/* Command line buffer */
static char command_buf[MAX_COMMAND];
static char *argv[MAX_ARGS];
static int argc;

/* Current working directory */
static char cwd[MAX_PATH] = "/";

/* Built-in commands */
typedef int (*builtin_cmd)(int argc, char *argv[]);

/* Forward declarations */
static int cmd_cd(int argc, char *argv[]);
static int cmd_pwd(int argc, char *argv[]);
static int cmd_echo(int argc, char *argv[]);
static int cmd_exit(int argc, char *argv[]);
static int cmd_help(int argc, char *argv[]);

static void refresh_cwd(void)
{
    if (ipc_getcwd(cwd, sizeof(cwd)) == 0) {
        strcpy(cwd, "/");
    }
}

struct builtin {
    const char *name;
    builtin_cmd func;
};

static struct builtin builtins[] = {
    {"cd", cmd_cd},
    {"pwd", cmd_pwd},
    {"echo", cmd_echo},
    {"exit", cmd_exit},
    {"help", cmd_help},
    {NULL, NULL}
};

/* Built-in command implementations */
static int cmd_cd(int argc, char *argv[])
{
    if (argc < 2) {
        if (ipc_chdir("/") != 0) {
            puts("cd: cannot change to root");
            return 1;
        }
    } else {
        if (ipc_chdir(argv[1]) == 0) {
            refresh_cwd();
        } else {
            puts("cd: cannot change to directory: ");
            puts(argv[1]);
            putchar('\n');
        }
    }
    return 0;
}

static int cmd_pwd(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    refresh_cwd();
    printf("%s\n", cwd);
    return 0;
}

static int cmd_echo(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            putchar(' ');
        }
    }
    putchar('\n');
    return 0;
}

static int cmd_exit(int argc, char *argv[])
{
    int status = 0;
    if (argc > 1) {
        status = atoi(argv[1]);
    }
    exit(status);
    return 0;
}

static int cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    printf("Muzix Shell - command interpreter for Muzix on Z80\n");
    printf("Built-in commands:\n");
    printf("  cd [dir]     - Change directory\n");
    printf("  pwd          - Print working directory\n");
    printf("  echo [args]  - Print arguments\n");
    printf("  help         - Show this help\n");
    printf("  exit [code]  - Exit shell\n\n");
    printf("External commands are executed using fork/exec:\n");
    printf("  ls [dir]    - List directory\n");
    printf("  cat file    - Print file contents\n");
    printf("  echo args   - Print arguments\n");
    printf("  date        - Show or set RTC date/time\n");
    printf("  top         - Process monitor\n");
    printf("  uptime      - Show system uptime\n");
    printf("  loadavg     - Show 1/5/15-minute load averages\n");
    printf("  hwdiag      - Hardware diagnostics\n");
    printf("  z80test     - Z80 CPU instruction tests\n");
    return 0;
}

/* Parse command line into arguments */
static int parse_command(char *line)
{
    char *p = line;
    argc = 0;
    
    /* The bound is MAX_ARGS - 1, not MAX_ARGS: argv[argc] = NULL below needs
     * one slot past the last argument, so a bound of MAX_ARGS let argc reach
     * 16 and the terminator was written to argv[16], one past the end of a
     * 16-element array.  Sixteen words of arguments were enough to smash the
     * stack.  Found by cppcheck, which is right about this one. */
    while (*p && argc < MAX_ARGS - 1) {
        /* Skip whitespace */
        while (*p && isspace(*p)) {
            p++;
        }
        
        if (*p == '\0') {
            break;
        }
        
        /* Start of argument */
        argv[argc++] = p;
        
        /* Find end of argument */
        while (*p && !isspace(*p)) {
            p++;
        }
        
        /* Null terminate */
        if (*p) {
            *p++ = '\0';
        }
    }
    
    argv[argc] = NULL;
    return argc;
}

/* Execute external command */
static int execute_external(int argc, char *argv[])
{
    int pid;
    int status = 0;
    char pathbuf[64];
    const char *path_prefixes[] = { "/bin/", "/sbin/", NULL };
    int i;

    (void)argc;  /* Suppress unused warning */

    pid = fork();

    if (pid == 0) {
        /* Child process: first try the command as-given, then PATH search */
        exec(argv[0], (const char * const *)argv);

        /* If exec fails, try PATH search */
        for (i = 0; path_prefixes[i] != NULL; i++) {
            /* Build path = prefix + argv[0] */
            char *p = pathbuf;
            const char *prefix = path_prefixes[i];
            while (*prefix && (p - pathbuf) < (int)sizeof(pathbuf) - 1) {
                *p++ = *prefix++;
            }
            const char *cmd = argv[0];
            while (*cmd && (p - pathbuf) < (int)sizeof(pathbuf) - 1) {
                *p++ = *cmd++;
            }
            *p = '\0';
            exec(pathbuf, (const char * const *)argv);
        }
        /* If exec fails, exit with error */
        puts("shell: cannot execute: ");
        puts(argv[0]);
        putchar('\n');
        exit(127);
        return 127;  /* Never reached, but satisfies compiler */
    } else if (pid > 0) {
        /* Parent process: wait for child */
        wait(&status);
        return status;
    } else {
        /* Fork failed */
        printf("shell: fork failed\n");
        return -1;
    }
}

/* Execute a command */
static int execute_command(int argc, char *argv[])
{
    if (argc == 0) {
        return 0;
    }
    
    /* Check for built-in commands */
    for (int i = 0; builtins[i].name != NULL; i++) {
        if (strcmp(argv[0], builtins[i].name) == 0) {
            return builtins[i].func(argc, argv);
        }
    }
    
    /* Try to execute external command */
    return execute_external(argc, argv);
}

/* Read a line of input */
static int readline(char *buf, int size)
{
    int i = 0;
    int c;

    while (i < size - 1) {
        char byte;
        int got;

        /* libc's read(), which parks on MUZIX_READ_AGAIN (-11) for 240 ticks
         * and returns every other answer, exactly as it does for getchar() -
         * the same path, one implementation of the park policy, rather than
         * this loop keeping a second copy of it in step.  That is the whole
         * change: the do/while below used to catch MUZIX_READ_AGAIN itself and
         * call sys_park(240), which is two places deciding when a console
         * reader waits and two places that have to agree about it.
         *
         * The three-way contract is unchanged and lands in the same test.
         * read() hands back a byte, or a zero, or a negative status, and only
         * "nothing yet" is absorbed inside it: a zero-length read is still
         * Ctrl-D and still ends the line; a negative answer is still a real
         * error - a bad descriptor, say - and retrying it for ever would be
         * the same spin with a worse excuse, so it ends the line too.  What
         * read() itself now guarantees is that -11 never reaches here, so the
         * old loop could not see it either and its removal costs nothing. */
        got = read(0, &byte, 1);
        if (got <= 0) {
            break;
        }
        c = (int)(unsigned char)byte;

        if (c == '\n') {
            buf[i++] = '\0';
            return i;
        }

        buf[i++] = (char)c;
    }

    buf[i] = '\0';
    return i;
}

/* Main shell loop */
static void shell_loop(void)
{
    printf("\n");
    printf("===========================================================\n");
    printf("Muzix Shell v1.0 - Muzix OS for Z80\n");
    /* Which tree this is.
     *
     * The ROM carries no version anywhere else and nothing in it changes when
     * the code does, so a machine on a bench cannot be asked what it is
     * running.  A short hash, with -dirty when the tree differed from that
     * commit when it was built - the number names the commit, and the flag says
     * whether that is in fact what ran.  "no-git" when the tree was built from
     * something that is not a checkout at all.
     *
     * It is the shell that prints this because the shell is PID 1 and lives in
     * the userspace text window, which has room; the kernel has 23 bytes of
     * _CODE left and could not afford a string it can never change. */
    printf("build %s\n", MUZIX_BUILD_ID);
    printf("Type 'help' for available commands, 'exit' to quit\n");
    printf("===========================================================\n\n");
    
    while (1) {
        /* Print prompt */
        refresh_cwd();
        printf("%s> ", cwd);
        /* The prompt has no newline, so it sits in libc's line buffer until
         * something else flushes it - and the next thing is the read() that
         * waits for a character.  Flush it here or the user sees nothing. */
        muzix_stdout_flush_now();
        
        /* Read command line */
        if (readline(command_buf, MAX_COMMAND) <= 0) {
            break;
        }
        
        /* Parse and execute */
        parse_command(command_buf);
        if (argc > 0) {
            execute_command(argc, argv);
        }
    }
    
    printf("Goodbye!\n");
}

/* Shell entry point */
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    
    shell_loop();
    return 0;
}
