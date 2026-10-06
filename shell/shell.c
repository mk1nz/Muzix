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
#include "muzix_build_stamp.h"

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
    if (!getcwd(cwd, sizeof(cwd))) {
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
        if (chdir("/") != 0) {
            puts("cd: cannot change to root");
            return 1;
        }
    } else {
        if (chdir(argv[1]) == 0) {
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
    printf("External commands are executed using fork/exec\n");
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
    
    (void)argc;  /* Suppress unused warning */
    
    pid = fork();
    
    if (pid == 0) {
        /* Child process: execute command */
        exec(argv[0], (const char * const *)argv);
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

        /* read(), not sys_read(): this is the process that has to wait, and
         * waiting means parking - taking this process out of the runnable set
         * so something else can run, and coming back when there is input.
         *
         * The spin this replaces was `sys_read` in a loop, because a read that
         * found nothing used to answer -1 and -1 means "no character", so the
         * only thing left to do with it was ask again, immediately, for as long
         * as the user declined to type.  That is the whole of the CPU
         * saturation: the prompt was idle and the processor was not, and the
         * load figure reporting 1.00 was the only honest thing it could say.
         *
         * read() parks instead, and returns when the device has something. */
        int32_t got = read(0, &byte, 1);

        /* read() only returns when it has an answer that is not "nothing yet".
         * A zero-length read is still Ctrl-D and still ends the line.  A
         * negative answer is now a real error rather than a slow console - a
         * bad descriptor, say - and retrying it for ever would be the same spin
         * with a worse excuse, so it ends the line too. */
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
