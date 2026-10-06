#define _GNU_SOURCE
#include <sched.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>

#include "rootfs.h"
#include "cgroup.h"
#include "seccomp.h"
#include "cap.h"

typedef struct {
    char **argv;
    char *rootfs;
    int slave_fd; // PTY: file descriptor for the slave end
    int pipefd;
} container_args;

static int create_user_namespaces(pid_t pid) {
    char map[256];
    char path[256];
    int fd;

    snprintf(map, sizeof(map), "0 %d 1\n", getuid());
    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("error writing uid_map");
        return 1;
    }

    write(fd, map, strlen(map));
    close(fd);

    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    fd = open(path, O_WRONLY);

    if (fd >= 0) {
        write(fd, "deny", 4);
        close(fd);
    }

    snprintf(map, sizeof(map), "0 %d 1\n", getgid());
    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);

    usleep(10000);

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("error writing gid_map");
        return 1;
    }

    write(fd, map, strlen(map));
    close(fd);

    return 0;
}

// CHILD
static int container_main(void *arg) {
    sleep(1);

    container_args *args = (container_args *)arg;

    // Wait for the parent to write the UID/GID mappings
    char c;
    read(args->pipefd, &c, 1);
    close(args->pipefd);

    sethostname("mcr", 3);
    setup_rootfs(args->rootfs);
    setsid();  // New session (without a controlling terminal yet)

    // PTY: assign the slave as the controlling terminal and redirect stdin/out/err
    if (args->slave_fd >= 0) {
        ioctl(args->slave_fd, TIOCSCTTY, 0);
        dup2(args->slave_fd, STDIN_FILENO);
        dup2(args->slave_fd, STDOUT_FILENO);
        dup2(args->slave_fd, STDERR_FILENO);

        if (args->slave_fd > STDERR_FILENO)
            close(args->slave_fd);
    }

    // Limit container memory (works without sudo)
    struct rlimit lim = {
        .rlim_cur = 128 * 1024 * 1024,
        .rlim_max = 128 * 1024 * 1024
    };

    setrlimit(RLIMIT_AS, &lim);

    if (reduce_capabilities() != 0) {
        fprintf(stderr, "reduce_capabilities failed\n");
        exit(1);
    }

    if (load_seccomp_filter() != 0) {
        fprintf(stderr, "Failed to load seccomp\n");
        exit(1);
    }

    execvp(args->argv[0], args->argv);
    perror("execvp failed");
    exit(1);
}

// PTY: loop that relays data between the real terminal and the master PTY
static void relay_pty(int master_fd) {
    struct termios orig, raw;

    tcgetattr(STDIN_FILENO, &orig);
    raw = orig;

    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char buf[4096];
    fd_set fds;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(master_fd, &fds);

        if (select(master_fd + 1, &fds, NULL, NULL, NULL) < 0)
            break;

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));

            if (n <= 0)
                break;

            write(master_fd, buf, n);
        }

        if (FD_ISSET(master_fd, &fds)) {
            ssize_t n = read(master_fd, buf, sizeof(buf));

            if (n <= 0)
                break;  // EIO = container closed its PTY

            write(STDOUT_FILENO, buf, n);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);  // Restore terminal
}

// PARENT
int create_container(char *rootfs, char **command_argv) {

    container_args args;
    args.argv = command_argv;
    args.rootfs = rootfs;

    // PTY: open master
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);

    if (master_fd == -1) {
        perror("posix_openpt");
        return 1;
    }

    grantpt(master_fd);
    unlockpt(master_fd);

    int slave_fd = open(ptsname(master_fd), O_RDWR | O_NOCTTY);

    if (slave_fd == -1) {
        perror("open slave PTY");
        close(master_fd);
        return 1;
    }

    args.slave_fd = slave_fd;

    // PTY: propagate window size
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        ioctl(master_fd, TIOCSWINSZ, &ws);

    char *stack = malloc(65536);

    if (!stack) {
        perror("malloc");
        close(master_fd);
        close(slave_fd);
        return 1;
    }

    // if (unshare(CLONE_NEWUSER) == -1) {
    //     perror("failed to create new user namespace");
    //     free(stack);
    //     close(master_fd);
    //     close(slave_fd);
    //     return 1;
    // }

    int pipefd[2];
    pipe(pipefd);

    args.pipefd = pipefd[0];  // Pass read fd to the child

    int flags = CLONE_NEWUSER |
                CLONE_NEWNS |
                CLONE_NEWUTS |
                CLONE_NEWPID |
                CLONE_NEWNET |
                SIGCHLD;

    pid_t pid = clone(
        container_main,
        stack + 65536,
        flags,
        &args
    );

    if (pid == -1) {
        perror("clone failed");
        free(stack);
        close(master_fd);
        close(slave_fd);
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    close(pipefd[0]);

    create_user_namespaces(pid);

    write(pipefd[1], "x", 1);  // Notify child that mappings are ready
    close(pipefd[1]);

    close(slave_fd);  // PTY: parent no longer needs the slave

    printf("Child PID: %d\n", pid);

    if (getuid() == 0) {
        printf("Calling create_cgroup...\n");
        create_cgroup("mcr", pid, "256M", "50000 100000", "10");
    } else {
        printf("[warning] without sudo: cgroups disabled\n");
    }

    // System resources when starting the container
    {
        struct sysinfo si;
        long cpus = sysconf(_SC_NPROCESSORS_ONLN);

        printf("---------------------------\n");

        if (sysinfo(&si) == 0) {
            unsigned long total_mb =
                si.totalram * si.mem_unit / (1024 * 1024);

            unsigned long free_mb =
                si.freeram * si.mem_unit / (1024 * 1024);

            printf("Available CPUs : %ld\n", cpus);
            printf("Total RAM      : %lu MB\n", total_mb);
            printf("Free RAM       : %lu MB\n", free_mb);
        }

        printf("---------------------------\n");
    }

    fflush(stdout);  // Flush prints before the relay takes control

    relay_pty(master_fd);  // PTY: blocks until the container exits

    waitpid(pid, NULL, 0);

    if (getuid() == 0)
        delete_cgroup(pid);

    close(master_fd);
    free(stack);

    return 0;
}