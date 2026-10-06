#define _GNU_SOURCE
#include "rootfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int setup_rootfs(char *rootfs) {
    char abs_rootfs[PATH_MAX];

    if (realpath(rootfs, abs_rootfs) == NULL) {
        perror("realpath failed");
        return 1;
    }

    // 1. Isolate the mount namespace from the host
    //    (may fail in user namespaces, not fatal)
    mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL);

    // 2. Bind mount the host's /dev into the new rootfs BEFORE pivot/chroot
    //    mount() does not require write permission on the destination directory,
    //    only CAP_SYS_ADMIN
    {
        char dev_path[PATH_MAX + 8];

        snprintf(dev_path, sizeof(dev_path), "%s/dev", abs_rootfs);
        mkdir(dev_path, 0755);  // May fail if it already exists, which is fine

        if (mount("/dev", dev_path, NULL, MS_BIND | MS_REC, NULL) == 0)
            mount(NULL, dev_path, NULL, MS_PRIVATE | MS_REC, NULL);
    }

    // 3. Bind mount the new rootfs onto itself (required for pivot_root)
    if (mount(abs_rootfs, abs_rootfs, NULL, MS_BIND | MS_REC, NULL) == -1) {
        perror("bind mount failed");
        return 1;
    }

    // 4. Refresh CWD to the top of the bind mount
    if (chdir(abs_rootfs) == -1) {
        perror("chdir after bind mount failed");
        return 1;
    }

    // 5. Try pivot_root
    //    If the rootfs belongs to root and we are running without sudo,
    //    mkdir("old_root") fails with EACCES inside the user namespace.
    //    Use chroot as a fallback.
    int pivot_ok = 0;

    if (mkdir("old_root", 0755) == 0) {
        if (syscall(SYS_pivot_root, ".", "old_root") == 0) {
            pivot_ok = 1;
            chdir("/");
            umount2("/old_root", MNT_DETACH);
            rmdir("/old_root");
        } else {
            rmdir("old_root");
        }
    }

    if (!pivot_ok) {
        // Fallback: chroot (works with CAP_SYS_CHROOT in a user namespace)
        if (chroot(abs_rootfs) == -1) {
            perror("chroot failed");
            return 1;
        }

        chdir("/");
    }

    // 6. Mount virtual filesystems
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    mkdir("/tmp", 0755);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);

    return 0;
}