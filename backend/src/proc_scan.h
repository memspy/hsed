/*
 * proc_scan.h — walks /proc/<pid>/fd looking for symlinks the kernel has
 * suffixed with " (deleted)": file descriptors that still hold disk blocks
 * alive for an inode that no longer has any directory entry. This is the
 * backend for the "df says 100%, du finds nothing" investigation.
 */
#ifndef HSED_PROC_SCAN_H
#define HSED_PROC_SCAN_H

#include <sys/types.h>
#include <time.h>

#define HSED_PATH_MAX 4096
#define HSED_NAME_MAX 256
#define HSED_CMDLINE_MAX 1024

typedef struct {
    pid_t pid;
    int fd;
    char mode[3];           /* "r", "w", "rw", or "?" */
    long long size;
    uid_t uid;
    char user[HSED_NAME_MAX];
    char comm[HSED_NAME_MAX];
    char cmdline[HSED_CMDLINE_MAX];
    char path[HSED_PATH_MAX];   /* original (now-deleted) path */
    unsigned long long inode;
    unsigned dev_major;
    unsigned dev_minor;
    time_t mtime;
} hsed_entry_t;

typedef struct {
    hsed_entry_t *items;
    size_t count;
    size_t capacity;
} hsed_list_t;

void hsed_list_init(hsed_list_t *list);
void hsed_list_free(hsed_list_t *list);

/*
 * Scans /proc for unlinked-but-open files.
 *   min_size  - skip entries smaller than this many bytes (0 = no filter)
 *   only_pid  - if > 0, scan only this PID; if <= 0, scan every process
 * Entries you don't have permission to inspect (a different uid's process,
 * without root/matching privileges) are silently skipped, same as lsof.
 * Returns 0 on success (even if zero entries were found), -1 on a hard
 * failure to even open /proc itself.
 */
int hsed_scan(hsed_list_t *out, long long min_size, pid_t only_pid);

#endif /* HSED_PROC_SCAN_H */
