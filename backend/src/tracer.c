#include "tracer.h"

#if defined(__x86_64__) || defined(__aarch64__)

#include <dirent.h>
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

#define POLL_SLEEP_US 15000        /* how often we check poll_cb while every tracee is idle */
#define MAX_CAPTURE_HARD_LIMIT 65536
#define MAX_TRACKED_TIDS 512       /* generous cap on threads followed at once per session */
#define MAX_WRITEV_IOVECS 16       /* iovecs inspected per writev() capture */

/* --- register access: the only genuinely architecture-specific part of
 * this file. Everything below this block (thread discovery/tracking, the
 * main ptrace loop, writev reassembly) is written purely in terms of
 * get_regs()/reg_syscall_nr()/reg_arg1()/reg_arg2()/reg_arg3()/reg_ret(),
 * so adding another architecture means implementing just these six
 * functions, not touching the tracing logic itself. ---
 *
 * x86_64 verified on real hardware (see the test suite in the repo).
 * aarch64 compiles and has been exercised for SCAN/TRUNCATE/HUP/KILL
 * under QEMU user-mode emulation, but STREAM's ptrace path has NOT been
 * verified on real ARM64 hardware — QEMU user-mode's ptrace emulation is
 * known to be unreliable for exactly this kind of syscall-stepping, so a
 * negative result there wouldn't mean much either way. The register
 * layout and calling convention below are correct per the kernel/psABI
 * documentation; treat this path as implemented-but-please-report-back
 * until someone confirms it on actual hardware. */
#if defined(__x86_64__)

typedef struct user_regs_struct hsed_regs_t;

static int get_regs(pid_t tid, hsed_regs_t *out) {
    return ptrace(PTRACE_GETREGS, tid, NULL, out) == 0 ? 0 : -1;
}
/* x86_64 quirk: the syscall number is only reliably readable from
 * orig_rax — rax itself is overwritten with the return value by the time
 * of the syscall-exit stop, so orig_rax is what stays valid across both
 * enter and exit stops for one syscall. */
static long long reg_syscall_nr(const hsed_regs_t *r) { return (long long)r->orig_rax; }
static long long reg_arg1(const hsed_regs_t *r) { return (long long)r->rdi; }
static unsigned long long reg_arg2(const hsed_regs_t *r) { return (unsigned long long)r->rsi; }
static unsigned long long reg_arg3(const hsed_regs_t *r) { return (unsigned long long)r->rdx; }
static long long reg_ret(const hsed_regs_t *r) { return (long long)r->rax; }

#elif defined(__aarch64__)
#include <elf.h> /* NT_PRSTATUS */

typedef struct user_regs_struct hsed_regs_t; /* regs[0..30], sp, pc, pstate (glibc's <sys/user.h>) */

static int get_regs(pid_t tid, hsed_regs_t *out) {
    struct iovec iov = { .iov_base = out, .iov_len = sizeof(*out) };
    return ptrace(PTRACE_GETREGSET, tid, (void *)(long)NT_PRSTATUS, &iov) == 0 ? 0 : -1;
}
/* AArch64 quirk (the opposite of x86_64's): the syscall number lives in
 * x8, which the kernel does NOT clobber for the syscall's return value
 * (that goes in x0 instead) — so plain x8 is valid at both enter and
 * exit, no "orig_x8" needed. */
static long long reg_syscall_nr(const hsed_regs_t *r) { return (long long)r->regs[8]; }
static long long reg_arg1(const hsed_regs_t *r) { return (long long)r->regs[0]; }
static unsigned long long reg_arg2(const hsed_regs_t *r) { return (unsigned long long)r->regs[1]; }
static unsigned long long reg_arg3(const hsed_regs_t *r) { return (unsigned long long)r->regs[2]; }
static long long reg_ret(const hsed_regs_t *r) { return (long long)r->regs[0]; }

#endif

static ssize_t read_remote(pid_t tid, unsigned long long addr, unsigned char *buf, size_t len) {
    struct iovec local = { .iov_base = buf, .iov_len = len };
    struct iovec remote = { .iov_base = (void *)(uintptr_t)addr, .iov_len = len };
    return process_vm_readv(tid, &local, 1, &remote, 1, 0); /* -1 on error, e.g. unmapped page */
}

/* Reassembles a writev() payload preview: reads the iovec array itself out
 * of the tracee, then reads from each segment's base in order,
 * concatenating into `capture` until max_capture total bytes are collected
 * (or MAX_WRITEV_IOVECS segments have been consulted, whichever first). */
static size_t capture_writev(pid_t tid, unsigned long long iov_addr, unsigned long long iovcnt,
                              unsigned char *capture, size_t max_capture) {
    if (iovcnt == 0) return 0;
    if (iovcnt > MAX_WRITEV_IOVECS) iovcnt = MAX_WRITEV_IOVECS;

    struct remote_iovec { unsigned long long base; unsigned long long len; };
    struct remote_iovec iovs[MAX_WRITEV_IOVECS];

    ssize_t n = read_remote(tid, iov_addr, (unsigned char *)iovs, iovcnt * sizeof(iovs[0]));
    if (n <= 0) return 0;
    size_t got_iovs = (size_t)n / sizeof(iovs[0]);

    size_t total = 0;
    for (size_t i = 0; i < got_iovs && total < max_capture; i++) {
        size_t want = (size_t)iovs[i].len;
        size_t space = max_capture - total;
        if (want > space) want = space;
        if (want == 0) continue;
        ssize_t got = read_remote(tid, iovs[i].base, capture + total, want);
        if (got > 0) total += (size_t)got;
    }
    return total;
}

/* Per-thread syscall-stepping state — one traced process can have many of
 * these live at once, each independently mid-syscall or not. */
typedef struct {
    pid_t tid;
    int in_syscall;          /* toggles: 0 = expecting enter-stop, 1 = expecting exit-stop */
    int capturing;           /* did the syscall we just entered target our fd? */
    int pending_is_writev;
    unsigned long long pending_buf_addr;
    unsigned long long pending_iovcnt;
} tid_state_t;

static tid_state_t *find_or_add_tid(tid_state_t *tids, int *ntids, pid_t tid, int *was_new) {
    for (int i = 0; i < *ntids; i++) {
        if (tids[i].tid == tid) {
            *was_new = 0;
            return &tids[i];
        }
    }
    if (*ntids >= MAX_TRACKED_TIDS) {
        *was_new = 0;
        return NULL; /* over capacity — extremely thread-heavy target; not tracked */
    }
    tid_state_t *t = &tids[(*ntids)++];
    memset(t, 0, sizeof(*t));
    t->tid = tid;
    *was_new = 1;
    return t;
}

static void remove_tid(tid_state_t *tids, int *ntids, pid_t tid) {
    for (int i = 0; i < *ntids; i++) {
        if (tids[i].tid == tid) {
            tids[i] = tids[*ntids - 1];
            (*ntids)--;
            return;
        }
    }
}

/* Lists every thread ID currently under /proc/<pid>/task — for a
 * thread-group leader this is every thread of the process; for any other
 * (non-leader) tid, the kernel only lists that tid itself, which is what
 * lets a caller narrow tracing to one specific thread (see tracer.h). */
static int discover_tids(pid_t pid, pid_t *out, int max_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
    DIR *d = opendir(path);
    if (!d) return -1;

    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char *end = NULL;
        long v = strtol(de->d_name, &end, 10);
        if (end == de->d_name || *end != '\0') continue;
        if (n < max_out) out[n++] = (pid_t)v;
    }
    closedir(d);
    return n;
}

int hsed_trace_fd(pid_t pid, int fd, size_t max_capture,
                   hsed_attached_fn attached_cb, hsed_poll_fn poll_cb,
                   hsed_write_fn write_cb, void *ctx,
                   char *errbuf, size_t errlen) {
    if (max_capture == 0) max_capture = 200;
    if (max_capture > MAX_CAPTURE_HARD_LIMIT) max_capture = MAX_CAPTURE_HARD_LIMIT;

    pid_t discovered[MAX_TRACKED_TIDS];
    int ndiscovered = discover_tids(pid, discovered, MAX_TRACKED_TIDS);
    if (ndiscovered <= 0) {
        snprintf(errbuf, errlen, "could not list threads for %d: %s", (int)pid, strerror(errno));
        return -1;
    }

    tid_state_t tids[MAX_TRACKED_TIDS];
    int ntids = 0;

    /* Seize every discovered thread individually — PTRACE_SEIZE attaches
     * without stopping the tracee as a side effect (unlike PTRACE_ATTACH),
     * and PTRACE_O_TRACECLONE makes any *new* thread this process creates
     * later auto-attach to us the same way, which is what gives us
     * strace -f-equivalent coverage of threads that don't exist yet. */
    for (int i = 0; i < ndiscovered; i++) {
        pid_t tid = discovered[i];

        /* A fast back-to-back STREAM/STREAM on the same target can race a
         * just-finished previous session's detach: our poll_cb-based
         * disconnect detection only notices the old client is gone on its
         * next ~15ms polling tick, so PTRACE_SEIZE here can transiently
         * fail with EPERM ("still traced by us, a moment ago") even
         * though nothing is actually wrong. A few short retries clear
         * this up almost immediately without changing behavior for a
         * genuine permissions failure (which keeps failing the same way,
         * just ~20ms later). */
        int seize_rc = -1;
        for (int attempt = 0; attempt < 5; attempt++) {
            seize_rc = ptrace(PTRACE_SEIZE, tid, NULL,
                               (void *)(long)(PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE));
            if (seize_rc == 0 || errno != EPERM) break;
            usleep(5000);
        }
        if (seize_rc != 0) {
            continue; /* raced away, or a genuine permissions failure — skip this thread */
        }
        if (ptrace(PTRACE_INTERRUPT, tid, NULL, NULL) != 0) {
            ptrace(PTRACE_DETACH, tid, NULL, NULL);
            continue;
        }
        int status;
        if (waitpid(tid, &status, 0) != tid || !WIFSTOPPED(status)) {
            ptrace(PTRACE_DETACH, tid, NULL, NULL);
            continue;
        }
        int was_new = 0;
        tid_state_t *ts = find_or_add_tid(tids, &ntids, tid, &was_new);
        if (!ts) {
            ptrace(PTRACE_DETACH, tid, NULL, NULL);
            continue;
        }
        if (ptrace(PTRACE_SYSCALL, tid, NULL, NULL) != 0) {
            remove_tid(tids, &ntids, tid);
            ptrace(PTRACE_DETACH, tid, NULL, NULL);
        }
    }

    if (ntids == 0) {
        snprintf(errbuf, errlen,
                 "PTRACE_SEIZE failed for every thread of %d: %s (need root/CAP_SYS_PTRACE, "
                 "and /proc/sys/kernel/yama/ptrace_scope must allow it)",
                 (int)pid, strerror(errno));
        return -1;
    }

    if (attached_cb) attached_cb(ctx); /* only now do we know we're genuinely attached */

    unsigned char *capture = malloc(max_capture);
    if (!capture) {
        snprintf(errbuf, errlen, "out of memory allocating a %zu-byte capture buffer", max_capture);
        for (int i = 0; i < ntids; i++) {
            if (ptrace(PTRACE_INTERRUPT, tids[i].tid, NULL, NULL) == 0) {
                int st;
                waitpid(tids[i].tid, &st, 0);
            }
            ptrace(PTRACE_DETACH, tids[i].tid, NULL, NULL);
        }
        return -1;
    }

    for (;;) {
        int status;
        pid_t r = waitpid(-1, &status, __WALL | WNOHANG);

        if (r == 0) {
            if (!poll_cb(ctx)) {
                for (int i = 0; i < ntids; i++) {
                    if (ptrace(PTRACE_INTERRUPT, tids[i].tid, NULL, NULL) == 0) {
                        int st;
                        waitpid(tids[i].tid, &st, 0);
                    }
                    ptrace(PTRACE_DETACH, tids[i].tid, NULL, NULL);
                }
                free(capture);
                return 0;
            }
            usleep(POLL_SLEEP_US);
            continue;
        }

        if (r == -1) {
            if (errno == ECHILD) { free(capture); return 0; } 
            continue; 
        }

        int was_new = 0;
        tid_state_t *ts = find_or_add_tid(tids, &ntids, r, &was_new);
        if (!ts) {
            ptrace(PTRACE_CONT, r, NULL, NULL);
            continue;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            remove_tid(tids, &ntids, r);
            if (ntids == 0) { free(capture); return 0; } 
            continue;
        }
        if (!WIFSTOPPED(status)) continue;

        if (was_new) {
            ptrace(PTRACE_SYSCALL, r, NULL, NULL);
            continue;
        }

        int stopsig = WSTOPSIG(status);

        if (stopsig == (SIGTRAP | 0x80)) {
            if (!ts->in_syscall) {
                hsed_regs_t regs;
                ts->capturing = 0;
                if (get_regs(r, &regs) == 0) {
                    long long scno = reg_syscall_nr(&regs);
                    if ((scno == SYS_write || scno == SYS_pwrite64 || scno == SYS_writev) &&
                        reg_arg1(&regs) == fd) {
                        ts->capturing = 1;
                        ts->pending_is_writev = (scno == SYS_writev);
                        ts->pending_buf_addr = reg_arg2(&regs);
                        ts->pending_iovcnt = ts->pending_is_writev ? reg_arg3(&regs) : 0;
                    }
                }
                ts->in_syscall = 1;
            } else {
                if (ts->capturing) {
                    hsed_regs_t regs;
                    if (get_regs(r, &regs) == 0) {
                        long long ret = reg_ret(&regs);
                        size_t got = 0;
                        if (ret > 0) {
                            if (ts->pending_is_writev) {
                                got = capture_writev(r, ts->pending_buf_addr,
                                                      ts->pending_iovcnt, capture, max_capture);
                            } else {
                                size_t want = (size_t)ret < max_capture ? (size_t)ret : max_capture;
                                ssize_t n = read_remote(r, ts->pending_buf_addr, capture, want);
                                if (n > 0) got = (size_t)n;
                            }
                        }
                        write_cb(ctx, r, (long)ret, capture, got, ts->pending_is_writev);
                    }
                }
                ts->in_syscall = 0;
                ts->capturing = 0;
            }
            ptrace(PTRACE_SYSCALL, r, NULL, NULL);
        } else if (stopsig == SIGTRAP) {
            int event = status >> 16;
            if (event == PTRACE_EVENT_CLONE) {
                unsigned long new_tid_ul = 0;
                if (ptrace(PTRACE_GETEVENTMSG, r, NULL, &new_tid_ul) == 0) {
                    int child_was_new = 0;
                    find_or_add_tid(tids, &ntids, (pid_t)new_tid_ul, &child_was_new);
                }
            }
            ptrace(PTRACE_SYSCALL, r, NULL, NULL);
        } else if (stopsig == SIGSTOP) {
            ptrace(PTRACE_SYSCALL, r, NULL, NULL);
        } else {
            ptrace(PTRACE_SYSCALL, r, NULL, (void *)(long)stopsig);
        }
    }
}

#else /* unsupported architecture */

int hsed_trace_fd(pid_t pid, int fd, size_t max_capture,
                   hsed_attached_fn attached_cb, hsed_poll_fn poll_cb,
                   hsed_write_fn write_cb, void *ctx,
                   char *errbuf, size_t errlen) {
    (void)pid; (void)fd; (void)max_capture; (void)attached_cb; (void)poll_cb;
    (void)write_cb; (void)ctx;
    snprintf(errbuf, errlen,
             "live streaming is only implemented for x86_64 and aarch64 in this version");
    return -1;
}

#endif
