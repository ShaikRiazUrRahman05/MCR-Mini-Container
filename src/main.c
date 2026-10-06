#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
//#include <sys/mount.h>
//#include <sys/syscall.h>
//#include <sys/wait.h>
//#include <linux/seccomp.h>
//#include <linux/filter.h>
//#include <sys/capability.h>

int create_container(char *rootfs, char **command_argv);

void show_help(char *program_name) {
    printf("Usage: %s run --rootfs <path> -- <command [args]>\n", program_name);
    printf("Example: %s run --rootfs ./alpine -- /bin/sh\n", program_name);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        show_help(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "run") != 0) {
        printf("Error: the first argument must be 'run'\n");
        show_help(argv[0]);
        return 1;
    }

    // Variables used to store parsed arguments
    char *rootfs = NULL;
    char *command = NULL;
    int command_index = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--rootfs") == 0) {
            if (i + 1 < argc) {
                rootfs = argv[i + 1];
                i = i + 1;
            } else {
                printf("Error: --rootfs requires a path\n");
                return 1;
            }
        }
        else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) {
                command = argv[i + 1];
                command_index = i + 1;
                break;
            } else {
                printf("Error: -- requires a command\n");
                return 1;
            }
        }
    }

    if (rootfs == NULL) {
        printf("Error: missing --rootfs\n");
        show_help(argv[0]);
        return 1;
    }

    char abs_rootfs[PATH_MAX];

    if (realpath(rootfs, abs_rootfs) == NULL) {
        perror("realpath failed in main");
        return 1;
    }

    printf("Absolute path: %s\n", abs_rootfs);

    if (command == NULL) {
        printf("Error: no command provided; use -- followed by a command\n");
        show_help(argv[0]);
        return 1;
    }

    printf("rootfs: %s\n", rootfs);
    printf("command: %s\n", command);

    for (int i = command_index; i < argc; i++) {
        printf("arg[%d]: %s\n", i - command_index, argv[i]);
    }

    create_container(abs_rootfs, &argv[command_index]);

    return 0;
}