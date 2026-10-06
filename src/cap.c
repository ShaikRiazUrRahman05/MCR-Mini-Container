#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/capability.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <errno.h>
#include "cap.h"

#define ACTIVATE_CAP(data, cap) \
    do { \
        (data)[0].effective   |= (1U << (cap)); \
        (data)[0].permitted   |= (1U << (cap)); \
        (data)[0].inheritable |= (1U << (cap)); \
    } while(0)

int reduce_capabilities(void) {
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct previous_data[2];
    struct __user_cap_data_struct new_data[2];

    header.version = 0x20080522;  // _LINUX_CAPABILITY_VERSION_3
    header.pid = 0;

    if (capget(&header, previous_data) == -1) {
        perror("capget failed");
        return 1;
    }

    new_data[0] = previous_data[0];
    new_data[1] = previous_data[1];

    new_data[0].effective = 0;
    new_data[0].permitted = 0;
    new_data[0].inheritable = 0;

    new_data[1].effective = 0;
    new_data[1].permitted = 0;
    new_data[1].inheritable = 0;

    // Only the capabilities required for basic isolation and user switching
    ACTIVATE_CAP(new_data, CAP_SYS_CHROOT);        // pivot_root / chroot
    ACTIVATE_CAP(new_data, CAP_SETUID);            // Change to a non-root user
    ACTIVATE_CAP(new_data, CAP_SETGID);            // Change to a non-root group
    ACTIVATE_CAP(new_data, CAP_KILL);              // Send signals to the child process
    ACTIVATE_CAP(new_data, CAP_NET_BIND_SERVICE);  // Allow binding to ports below 1024
    ACTIVATE_CAP(new_data, CAP_DAC_OVERRIDE);      // Access files inside the rootfs

    if (capset(&header, new_data) == -1) {
        perror("capset failed");
        return 1;
    }

    return 0;
}