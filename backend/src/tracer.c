#include "tracer.h"

#if defined(__x86_64__)

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define POLL_SLEEP_US 15000        /* how often we check poll_cb while the target is idle */
#define MAX_CAPTURE_HARD_LIMIT 65536

static ssize_t read_remote(pid_t pid, unsigned long long addr, unsigned char *buf, size_t len) {
    struct iovec local = { .iov_base = buf, .iov_len = len };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)addr, .iov_len = len };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0); /* -1 on error, e.g. unmapped page */
}

int hsed_trace_fd(pid_t pid, int fd, size_t max_capture,
                   hsed_attached_fn attached_cb, hsed_poll_fn poll_cb,
                   hsed_write_fn write_cb, void *ctx,
                   char *errbuf, size_t errlen) {
    if (max_capture == 0) max_capture = 200;
    if (max_capture > MAX_CAPTURE_HARD_LIMIT) max_capture = MAX_CAPTURE_HARD_LIMIT;

    /* PTRACE_SEIZE attaches without stopping the tracee (unlike
     * PTRACE_ATTACH, which implicitly sends a stop). Options are passed
     * directly as the data argument. */
    if (ptrace(PTRACE_SEIZE, pid, NULL, (void *)(long)PTRACE_O_TRACESYSGOOD) != 0) {
        snprintf(errbuf, errlen,
                 "PTRACE_SEIZE(%d) failed: %s (need root/CAP_SYS_PTRACE, and "
                 "/proc/sys/kernel/yama/ptrace_scope must allow it)",
                 (int)pid, strerror(errno));
        return -1;
    }

    /* We need one initial stop before we can start syscall-stepping.
     * PTRACE_INTERRUPT forces that regardless of what the tracee is
     * currently doing. */
    int status;
    if (ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        snprintf(errbuf, errlen, "could not bring process %d to an initial stop: %s",
                 (int)pid, strerror(errno));
        ptrace(PTRACE_DETACH, pid, NULL, NULL); /* best-effort; may no-op */
        return -1;
    }

    if (attached_cb) attached_cb(ctx); /* only now do we know we're genuinely attached */

    unsigned char *capture = malloc(max_capture);
    if (!capture) {
        snprintf(errbuf, errlen, "out of memory allocating a %zu-byte capture buffer", max_capture);
        ptrace(PTRACE_INTERRUPT, pid, NULL, NULL);
        waitpid(pid, &status, 0);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    int in_syscall = 0;            /* toggles: 0 = expecting enter-stop, 1 = expecting exit-stop */
    int capturing = 0;             /* did the syscall we just entered target our fd? */
    int pending_is_writev = 0;
    unsigned long long pending_buf_addr = 0;
    long inject_sig = 0;           /* a real signal to re-deliver to the tracee, if any */

    for (;;) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)inject_sig) != 0) {
            break; /* most likely the process just exited between stops */
        }
        inject_sig = 0;

        pid_t r;
        int stop_requested = 0;
        for (;;) {
            r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            if (r == -1) {
                free(capture);
                return 0; /* tracee gone (ECHILD) - clean stop */
            }
            if (!poll_cb(ctx)) { stop_requested = 1; break; }
            usleep(POLL_SLEEP_US);
        }

        if (stop_requested) {
            /* The tracee is currently running (that's exactly why we're in
             * this branch instead of the r==pid one) — PTRACE_DETACH only
             * works on a stopped tracee, so force a stop first. Without
             * this step the detach below would silently fail (ESRCH) and
             * leave the process permanently attached to us, breaking any
             * future STREAM against it. */
            if (ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) == 0) {
                waitpid(pid, &status, 0);
            }
            ptrace(PTRACE_DETACH, pid, NULL, NULL);
            free(capture);
            return 0;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            free(capture);
            return 0; /* target process ended; nothing left to detach */
        }
        if (!WIFSTOPPED(status)) continue;

        int stopsig = WSTOPSIG(status);
        if (stopsig != (SIGTRAP | 0x80)) {
            /* Not our syscall-stop — either a PTRACE_EVENT stop (reported
             * with SIGTRAP and extra high bits we don't otherwise expect
             * here) or a real signal aimed at the tracee (e.g. an admin
             * sent SIGTERM). Re-inject genuine signals on the next
             * PTRACE_SYSCALL so we don't silently swallow them, then keep
             * tracing. */
            if (stopsig != SIGTRAP && stopsig != SIGSTOP) {
                inject_sig = stopsig;
            }
            continue;
        }

        if (!in_syscall) {
            /* syscall-enter */
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) { in_syscall = 1; continue; }

            long long scno = (long long)regs.orig_rax;
            capturing = 0;
            if ((scno == SYS_write || scno == SYS_pwrite64 || scno == SYS_writev) &&
                (long long)regs.rdi == fd) {
                capturing = 1;
                pending_is_writev = (scno == SYS_writev);
                pending_buf_addr = regs.rsi;
            }
            in_syscall = 1;
        } else {
            /* syscall-exit */
            if (capturing) {
                struct user_regs_struct regs;
                if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) == 0) {
                    long long ret = (long long)regs.rax;
                    size_t got = 0;
                    if (ret > 0 && !pending_is_writev) {
                        size_t want = (size_t)ret < max_capture ? (size_t)ret : max_capture;
                        ssize_t n = read_remote(pid, pending_buf_addr, capture, want);
                        if (n > 0) got = (size_t)n;
                    }
                    write_cb(ctx, pid, (long)ret, capture, got, pending_is_writev);
                }
            }
            in_syscall = 0;
            capturing = 0;
        }
    }

    free(capture);
    ptrace(PTRACE_DETACH, pid, NULL, NULL); /* best-effort; no-op if already gone */
    return 0;
}

#else /* !__x86_64__ */

int hsed_trace_fd(pid_t pid, int fd, size_t max_capture,
                   hsed_attached_fn attached_cb, hsed_poll_fn poll_cb,
                   hsed_write_fn write_cb, void *ctx,
                   char *errbuf, size_t errlen) {
    (void)pid; (void)fd; (void)max_capture; (void)attached_cb; (void)poll_cb;
    (void)write_cb; (void)ctx;
    snprintf(errbuf, errlen, "live streaming is only implemented for x86_64 in this version");
    return -1;
}

#endif
