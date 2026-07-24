/*
 * tracer.h — the "killer feature": attach to a running process via
 * ptrace(2) and stream, live, exactly what it writes into one specific
 * file descriptor.
 *
 * This is deliberately the same mechanism strace/gdb use (PTRACE_ATTACH +
 * single-stepping through syscalls with PTRACE_SYSCALL), scoped down to
 * just the write-family syscalls on one fd. No kernel module, no code
 * injection into the target — only the standard ptrace(2) debugging
 * interface, which requires the same privileges (root, or CAP_SYS_PTRACE
 * plus a permissive /proc/sys/kernel/yama/ptrace_scope) that gdb -p and
 * strace -p already require.
 *
 * Scope of this version:
 *   - x86_64 only (register layout in tracer.c is arch-specific)
 *   - traces exactly the PID/TID given — it does NOT follow clone()'d
 *     threads the way `strace -f` does. If the fd could be written by a
 *     different thread of a multi-threaded target, find that thread's TID
 *     under /proc/<pid>/task/ and pass it as `pid` instead.
 *   - captures write() and pwrite64() payloads; writev() calls are
 *     reported (syscall + length) but the payload preview is not captured
 *     in this version.
 */
#ifndef HSED_TRACER_H
#define HSED_TRACER_H

#include <sys/types.h>
#include <stddef.h>

/* Called once, right after we've genuinely attached to the tracee (before
 * the syscall-tracing loop starts) — the correct moment for the caller to
 * tell its client "attached", rather than announcing it optimistically
 * before we know PTRACE_SEIZE actually succeeded. */
typedef void (*hsed_attached_fn)(void *ctx);

/* Called periodically (roughly every ~15ms while the target is idle) so
 * the caller can check for e.g. a disconnected client. Return 0 to make
 * hsed_trace_fd() detach and return; return non-zero to keep going. */
typedef int (*hsed_poll_fn)(void *ctx);

/* Called once for every captured write aimed at the traced fd.
 *   tid       - the thread that made the call
 *   ret       - the syscall's return value (bytes actually written, or a
 *               negative -errno if it failed)
 *   buf/buflen- the captured preview of the buffer (buflen may be less
 *               than `ret` if the write was larger than max_capture, and
 *               is 0 for writev in this version)
 *   is_writev - 1 if this event came from a writev() call */
typedef void (*hsed_write_fn)(void *ctx, pid_t tid, long ret,
                               const unsigned char *buf, size_t buflen,
                               int is_writev);

/*
 * Attaches to `pid` (via PTRACE_SEIZE, which — unlike PTRACE_ATTACH —
 * doesn't stop the tracee as a side effect), traces write()/pwrite64()/
 * writev() calls that target `fd`, and invokes write_cb for each one.
 * Calls attached_cb once, right after attaching succeeds. Keeps running
 * until: the target exits, poll_cb returns 0, or an unrecoverable ptrace
 * error occurs. When asked to stop, uses PTRACE_INTERRUPT to force the
 * tracee to a safe stop point before detaching — PTRACE_DETACH requires
 * the tracee to actually be stopped, and it usually isn't at the moment
 * poll_cb says "stop" (it's mid-flight, running normally between syscall
 * stops), so skipping this step would silently fail to detach and leave
 * the process permanently attached, blocking any future STREAM on it.
 *
 * Returns 0 on a clean stop (target exited or poll_cb asked to stop), -1
 * if attaching failed at all (errbuf explains why — most commonly EPERM:
 * need root/CAP_SYS_PTRACE, or a restrictive ptrace_scope).
 */
int hsed_trace_fd(pid_t pid, int fd, size_t max_capture,
                   hsed_attached_fn attached_cb, hsed_poll_fn poll_cb,
                   hsed_write_fn write_cb, void *ctx,
                   char *errbuf, size_t errlen);

#endif /* HSED_TRACER_H */
