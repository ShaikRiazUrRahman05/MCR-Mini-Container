# Why the Container Was Dying Silently

## The Symptom

```text
Adding PID 123884 to the cgroup...

Removing cgroup for PID 123884...   ← immediate, no shell
```

The cgroup was being created and removed almost immediately. The `/ #` prompt never appeared.

---

## Layer-by-Layer Diagnosis

### Layer 1 — Was it the cgroup?

No. The cgroup was being removed _after_ `waitpid()`, which is the correct behavior.

The problem was that `waitpid()` was returning immediately because the child process had already died.

### Layer 2 — Was the process dying before `exec`?

This was tested with `/bin/echo "hello"` and `/bin/sh -c "echo inside"`.

Both worked. Conclusion: the problem was specific to the **interactive shell**.

### Layer 3 — Was it the lack of a TTY?

Yes, partially. Without a PTY, `ash` detects that it does not have a controlling terminal and exits immediately.

**Partial solution:** implement a PTY using `posix_openpt` + a parent↔child relay.

But even after implementing the PTY, the shell was still dying.

### Layer 4 — Was it seccomp?

This was the real bug. The order in `container_main` was:

```c
// INCORRECT ORDER

load_seccomp_filter();   // filter active

setsid();                // ← __NR_setsid was NOT in the whitelist → SIGSYS → dead

ioctl(TIOCSCTTY, ...);

execvp("/bin/sh", ...);
```

`SECCOMP_RET_KILL` kills the process without a message, without a catchable signal, and without a visible log.

The child was dying before it could reach `execvp`.

**First fix:** move `setsid()` and the PTY setup **before** applying seccomp.

But the shell was still dying.

### Layer 5 — Missing syscalls in the whitelist

After fixing the order, `ash` was able to start executing but died during its initialization.

The syscalls that `ash` needs during startup and that were **missing from the filter** were:

| Syscall       | No. | What `ash` uses it for                                 |
| ------------- | --: | ------------------------------------------------------ |
| `getpgid`     | 121 | Job control: obtaining the process group of a PID      |
| `getsid`      | 124 | Checking the current session                           |
| `getresuid`   | 118 | Reading real/effective/saved UID                       |
| `getresgid`   | 120 | Reading real/effective/saved GID                       |
| `sigaltstack` | 131 | Alternate signal stack (libc startup)                  |
| `statx`       | 332 | Modern version of `stat` used by musl on kernels 4.11+ |

---

## Why Was It So Difficult to Detect?

### 1. `SECCOMP_RET_KILL` Leaves No Visible Trace

When seccomp blocks a syscall with `SECCOMP_RET_KILL`:

- The process dies with `SIGSYS`.
- There is no message on stderr.
- There is no core dump by default.
- The parent only sees that the child exited due to a signal.
- `"execvp no child"` is never printed because the process dies before or during `execvp`.

### 2. The PTY Was Hiding the Error Output

After:

```c
dup2(slave_fd, STDERR_FILENO);
```

any error from the child was redirected to the PTY slave.

The parent's `relay_pty` did not start until after `create_cgroup` (~50 ms).

By that time, the child had already died, and the buffered PTY output was lost when the HUP occurred.

### 3. The Partial Tests Were Working

`/bin/echo` and `/bin/sh -c "echo"` passed through seccomp because they are non-interactive commands:

- They do not call `setpgid`/`getpgid` for job control.
- They do not configure a terminal.
- They do not read shell profile files.

Only interactive `ash` activates the full initialization sequence that uses the blocked syscalls.

### 4. The Bug Had Two Independent, Chained Causes

```text
Without PTY  → ash exits immediately (no visible error)

With PTY     → ash dies because of seccomp (no visible error)
```

Fixing the PTY without knowing about the seccomp issue made the external behavior look identical, which made it difficult to determine whether progress had actually been made.

---

## The Final Solution

### In `ns.c` — Correct Order in `container_main`

```c
sethostname("mcr", 3);

setup_rootfs(args->rootfs);

setsid();                          // 1. Create a new session

ioctl(args->slave_fd, TIOCSCTTY);  // 2. Assign the controlling TTY

dup2(args->slave_fd, STDIN_FILENO);

dup2(args->slave_fd, STDOUT_FILENO);

dup2(args->slave_fd, STDERR_FILENO);

load_seccomp_filter();             // 3. Apply seccomp LAST, after setup

execvp(args->argv[0], args->argv);
```

### In `seccomp.c` — Expand the Whitelist

Add the syscalls that `ash` and `musl` need during startup:

```c
ALLOW_SYSCALL(__NR_setpgid),

ALLOW_SYSCALL(__NR_getpgrp),

ALLOW_SYSCALL(__NR_getpgid),

ALLOW_SYSCALL(__NR_getsid),

ALLOW_SYSCALL(__NR_setsid),

ALLOW_SYSCALL(__NR_getresuid),

ALLOW_SYSCALL(__NR_getresgid),

ALLOW_SYSCALL(__NR_sigaltstack),

ALLOW_SYSCALL(__NR_statx),
```

---

## Result

```text
/ #
```

The interactive shell now starts successfully.
