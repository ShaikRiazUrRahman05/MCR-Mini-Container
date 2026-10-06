#define _GNU_SOURCE
#include "utils.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

int create_cgroup(char *name, pid_t pid, char *mem_max, char *cpu_max, char *pids_max) {
    char path[256];
    (void)name;

    printf("Creating cgroup structure for PID %d...\n", pid);

    // BEFORE creating /minicr, enable controllers at the root
    write_to_file("/sys/fs/cgroup/cgroup.subtree_control", "+memory +pids +cpu");

    // Then create the parent
    mkdir("/sys/fs/cgroup/minicr", 0755);

    // Then enable controllers in the parent
    write_to_file("/sys/fs/cgroup/minicr/cgroup.subtree_control",
                  "+memory +pids +cpu");

    // 0. Create parent cgroup (our own hierarchy)
    const char *parent_cgroup = "/sys/fs/cgroup/minicr";

    if (mkdir(parent_cgroup, 0755) == -1) {
        if (errno != EEXIST) {
            perror("Failed to create parent cgroup");
            return 1;
        }
        printf("Parent cgroup already exists, continuing...\n");
    }

    // 1. Enable controllers in the parent
    printf("Enabling controllers in parent cgroup...\n");
    write_to_file("/sys/fs/cgroup/minicr/cgroup.subtree_control",
                  "+memory +pids +cpu");

    // 2. Create the child cgroup directory
    snprintf(path, sizeof(path), "/sys/fs/cgroup/minicr/mcr-%d", pid);
    printf("Creating child cgroup: %s\n", path);

    if (mkdir(path, 0755) == -1) {
        if (errno != EEXIST) {
            perror("Failed to create child cgroup");
            return 1;
        }
    }

    // 3. Configure memory limit
    if (mem_max != NULL) {
        snprintf(path, sizeof(path),
                 "/sys/fs/cgroup/minicr/mcr-%d/memory.max", pid);
        printf("Setting memory limit: %s\n", mem_max);

        if (access(path, W_OK) == 0)
            write_to_file(path, mem_max);
    }

    // 4. Configure CPU limit
    if (cpu_max != NULL) {
        snprintf(path, sizeof(path),
                 "/sys/fs/cgroup/minicr/mcr-%d/cpu.max", pid);
        printf("Setting CPU limit: %s\n", cpu_max);

        if (access(path, W_OK) == 0)
            write_to_file(path, cpu_max);
    }

    // 5. Configure process limit
    if (pids_max != NULL) {
        snprintf(path, sizeof(path),
                 "/sys/fs/cgroup/minicr/mcr-%d/pids.max", pid);
        printf("Setting PID limit: %s\n", pids_max);

        if (access(path, W_OK) == 0)
            write_to_file(path, pids_max);
    }

    // 6. Add the process to the cgroup
    snprintf(path, sizeof(path),
             "/sys/fs/cgroup/minicr/mcr-%d/cgroup.procs", pid);

    printf("Adding PID %d to cgroup...\n", pid);

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d\n", pid);

    write_to_file(path, pid_str);

    return 0;
}

int delete_cgroup(pid_t pid) {
    char path[256];

    snprintf(path, sizeof(path),
             "/sys/fs/cgroup/minicr/mcr-%d", pid);

    if (rmdir(path) == -1) {
        perror("Failed to delete cgroup");
        return 1;
    }

    return 0;
}