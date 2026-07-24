/*
 * reclaim.h — the two ways to get disk space back from a hidden file
 * without restarting the process that's holding it open.
 */
#ifndef HSED_RECLAIM_H
#define HSED_RECLAIM_H

#include <sys/types.h>
#include <stddef.h>

/*
 * Force-truncates the unlinked file behind /proc/<pid>/fd/<fd> to zero
 * bytes, freeing its disk blocks immediately. The target process is never
 * signaled, paused, or restarted.
 *
 * Safe for append-only writers (typical log files). NOT generally safe for
 * files the process has mmap()'d or accesses at arbitrary offsets
 * (database data files, WAL segments, indexes) — truncating those can
 * raise SIGBUS on the process's next access or corrupt on-disk structures.
 * Callers should prefer hsed_reopen_signal() first for anything that isn't
 * clearly an append-only log.
 *
 * On success returns 0 and sets *freed_out to the number of bytes that
 * were freed. On failure returns -1 and writes a message into errbuf.
 */
int hsed_truncate_fd(pid_t pid, int fd, long long *freed_out,
                      char *errbuf, size_t errlen);

/*
 * Sends a log-reopen signal (SIGHUP by default) to `pid`. Well-behaved
 * daemons close and reopen their log files on this signal, releasing the
 * unlinked inode themselves — the safe-first alternative to truncate.
 * `sig` is a signal number (e.g. SIGHUP, SIGUSR1, SIGUSR2).
 */
int hsed_send_signal(pid_t pid, int sig, char *errbuf, size_t errlen);

#endif /* HSED_RECLAIM_H */
