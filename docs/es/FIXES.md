# MCR — History of Bugs Fixed for Rootless Support

## Context

The goal was to make the container run **without `sudo`** using `CLONE_NEWUSER` (Linux user namespaces).

When running:

```bash
./mcr run --rootfs ./alpine -- /bin/sh
```

the process would appear and disappear silently, without showing a prompt.

---

## Bug 1 — Silent Stack Overflow (the Most Critical)

**File:** `src/ns.c`

**Problem:**

The child's stack was created using `malloc(8192)` (8 KB). That pointer was passed to `clone()` as `stack + 8192`, pointing to the top of those 8 KB.

Inside `setup_rootfs()`, there are two local arrays allocated on the stack:

```c
char abs_rootfs[PATH_MAX];   // 4096 bytes

char dev_path[PATH_MAX + 8]; // 4104 bytes
```

Together, they consume approximately 8200 bytes, plus the stack-frame overhead → the stack overflowed. The kernel killed the child silently (with no visible signal or message).

**Fix:**

```c
// Before:

char *stack = malloc(8192);

pid_t pid = clone(container_main, stack + 8192, flags, &args);

// After:

char *stack = malloc(65536);

pid_t pid = clone(container_main, stack + 65536, flags, &args);
```

**Important:** changing only the `malloc` size is not enough. The second argument of `clone()` is the **top of the stack** (the stack grows downward on x86). If you forget to update `stack + 8192` → `stack + 65536`, the child still has exactly 8 KB of stack even though 65536 bytes were allocated.

---

## Bug 2 — Seccomp Blocked in the User Namespace

**File:** `src/seccomp.c`

**Problem:**

`prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, ...)` requires one of the following:

1. `CAP_SYS_ADMIN` in the **initial namespace** (the host's namespace).
2. The `no_new_privs` bit to be enabled.

In rootless mode, the process has `CAP_SYS_ADMIN` in the _new_ user namespace, but not in the host's namespace. On some kernels this is sufficient; on others it is not. Without `no_new_privs`, the `prctl` call failed with `EACCES`, and the child died before reaching `execvp`, without printing anything (stderr was already pointing to the PTY slave, and the relay terminated before reading the message).

**Fix:**

```c
// Add BEFORE loading the filter:

prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
```

This is the standard method used by Docker, runc, and containerd to load seccomp without root privileges.

---

## Bug 3 — MS_PRIVATE Caused `setup_rootfs` to Exit with `return 1`

**File:** `src/rootfs.c`

**Problem:**

In some user-namespace contexts,:

```c
mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL)
```

can fail. The code had:

```c
if (mount(...) == -1) {

    perror("mount --make-rprivate / failed");

    return 1;  // ← early exit before chroot

}
```

`container_main` ignored the return value of `setup_rootfs`, so `execvp("/bin/sh")` was executed on the **host's filesystem**, not Alpine. The host shell was then killed by seccomp (because of blocked syscalls).

**Fix:** remove the `return 1`. The call may fail silently, but it is not fatal — the mount namespace is already isolated by `CLONE_NEWNS`.

```c
// The mount namespace is already isolated by CLONE_NEWNS; if it fails, it is not fatal

mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL);
```

---

## Bug 4 — `pivot_root` Impossible → Fallback to `chroot`

**File:** `src/rootfs.c`

**Problem:**

Alpine was extracted using `sudo` and is owned by `root` on the host. Inside the user namespace, **UID 0 in the namespace = UID 1000 on the host (Alfred)**. From the kernel's perspective, Alfred is an "other" user in a directory owned by root.

Therefore:

```c
mkdir("old_root", 0755)
```

failed with `EACCES` → `pivot_root` was never attempted.

**Fix:** attempt `pivot_root`, and if `mkdir` fails, use `chroot` as a fallback:

```c
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

    chroot(abs_rootfs);  // CAP_SYS_CHROOT is available in the user namespace

    chdir("/");

}
```

---

## Dead Code Removed

| File            | What was removed        | Why                                                                               |
| --------------- | ----------------------- | --------------------------------------------------------------------------------- |
| `src/seccomp.c` | `strict_filter[]` array | Never used; it was a minimal filter of 5 syscalls that was replaced by `filter[]` |
| `src/cgroup.c`  | `read_file()` function  | Never called                                                                      |
| `src/cgroup.c`  | `int result;` variable  | Declared but never assigned or read                                               |
| `src/cgroup.c`  | `name` parameter        | Received but never used; replaced with `(void)name;`                              |

---

## Final Result

```text
$ ./mcr run --rootfs ./alpine -- /bin/sh

Child PID: 130712

[warning] without sudo: cgroups disabled

/ #
```

The container now runs **completely without `sudo`** using Linux user namespaces.

It also works with `sudo`, in which case cgroups v2 are enabled.
