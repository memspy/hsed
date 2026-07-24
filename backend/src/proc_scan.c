#include "proc_scan.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define DELETED_SUFFIX " (deleted)"
#define DELETED_SUFFIX_LEN 10

void hsed_list_init(hsed_list_t *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void hsed_list_free(hsed_list_t *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int list_push(hsed_list_t *list, const hsed_entry_t *e) {
    if (list->count == list->capacity) {
        size_t newcap = list->capacity == 0 ? 64 : list->capacity * 2;
        hsed_entry_t *grown = realloc(list->items, newcap * sizeof(hsed_entry_t));
        if (!grown) return -1;
        list->items = grown;
        list->capacity = newcap;
    }
    list->items[list->count++] = *e;
    return 0;
}

static int is_all_digits(const char *s) {
    if (!*s) return 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
    }
    return 1;
}

/* Tiny local strlcpy — always NUL-terminates dst (dstsize must be > 0). */
static size_t hsed_strlcpy_local(char *dst, const char *src, size_t dstsize) {
    size_t srclen = strlen(src);
    if (dstsize > 0) {
        size_t copy = srclen < dstsize - 1 ? srclen : dstsize - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

static void read_first_line(const char *path, char *buf, size_t buflen) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, (int)buflen, f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    }
    fclose(f);
}

static void read_cmdline(pid_t pid, char *buf, size_t buflen) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);

    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) goto fallback;

    char raw[HSED_CMDLINE_MAX];
    ssize_t n = read(fd, raw, sizeof(raw) - 1);
    close(fd);
    if (n <= 0) goto fallback;
    raw[n] = '\0';

    /* argv is NUL-separated; join with spaces into buf, bounded. */
    size_t out_pos = 0;
    int wrote_any = 0;
    for (ssize_t i = 0; i < n; ) {
        size_t arglen = strlen(raw + i);
        if (arglen > 0) {
            if (wrote_any && out_pos + 1 < buflen) buf[out_pos++] = ' ';
            size_t copy = arglen;
            if (out_pos + copy >= buflen) copy = buflen - out_pos - 1;
            if (out_pos < buflen - 1) {
                memcpy(buf + out_pos, raw + i, copy);
                out_pos += copy;
            }
            wrote_any = 1;
        }
        i += (ssize_t)arglen + 1;
        if (arglen == 0) i = n; /* avoid infinite loop on stray data */
    }
    buf[out_pos < buflen ? out_pos : buflen - 1] = '\0';
    if (wrote_any) return;

fallback:
    {
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid);
        read_first_line(comm_path, buf, buflen);
        if (buf[0] == '\0') hsed_strlcpy_local(buf, "?", buflen);
    }
}

static void owner_name(uid_t uid, char *buf, size_t buflen) {
    struct passwd pwbuf, *result = NULL;
    char scratch[1024];
    int rc = getpwuid_r(uid, &pwbuf, scratch, sizeof(scratch), &result);
    if (rc == 0 && result) {
        hsed_strlcpy_local(buf, result->pw_name, buflen);
    } else {
        snprintf(buf, buflen, "%u", (unsigned)uid);
    }
}

static void fd_mode(pid_t pid, int fdnum, char *out3) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", (int)pid, fdnum);
    strcpy(out3, "?");

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "flags:", 6) == 0) {
            long flags = strtol(line + 6, NULL, 8); /* octal, like /proc docs */
            long acc = flags & 3; /* O_ACCMODE */
            if (acc == 0) strcpy(out3, "r");
            else if (acc == 1) strcpy(out3, "w");
            else if (acc == 2) strcpy(out3, "rw");
            break;
        }
    }
    fclose(f);
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int is_virtual_target(const char *path) {
    return starts_with(path, "pipe:") ||
           starts_with(path, "socket:") ||
           starts_with(path, "anon_inode:") ||
           starts_with(path, "/memfd:") ||
           strcmp(path, "/dev/zero") == 0;
}

/* Scans one process's /proc/<pid>/fd directory and appends every
 * unlinked-but-open entry found to `out`. Returns 0 on success, including
 * when the process has no fd directory (already exited) or we lack
 * permission — those are not errors, just nothing to report. */
static int scan_one_pid(hsed_list_t *out, pid_t pid, long long min_size) {
    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", (int)pid);

    DIR *d = opendir(fd_dir);
    if (!d) return 0; /* exited or no permission — not an error */

    char comm_path[64], comm[HSED_NAME_MAX];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid);
    read_first_line(comm_path, comm, sizeof(comm));
    if (comm[0] == '\0') hsed_strlcpy_local(comm, "?", sizeof(comm));

    char cmdline[HSED_CMDLINE_MAX];
    int cmdline_loaded = 0;

    uid_t owner_uid = 0;
    char owner_buf[HSED_NAME_MAX] = "?";
    {
        char proc_path[32];
        snprintf(proc_path, sizeof(proc_path), "/proc/%d", (int)pid);
        struct stat pst;
        if (stat(proc_path, &pst) == 0) {
            owner_uid = pst.st_uid;
            owner_name(owner_uid, owner_buf, sizeof(owner_buf));
        }
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_all_digits(de->d_name)) continue;
        int fdnum = atoi(de->d_name);

        char link_path[64 + 256 + 2];
        snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir, de->d_name);

        char target[HSED_PATH_MAX];
        ssize_t tlen = readlink(link_path, target, sizeof(target) - 1);
        if (tlen < 0) continue;
        target[tlen] = '\0';

        if (tlen < DELETED_SUFFIX_LEN) continue;
        if (strcmp(target + tlen - DELETED_SUFFIX_LEN, DELETED_SUFFIX) != 0) continue;
        target[tlen - DELETED_SUFFIX_LEN] = '\0'; /* strip " (deleted)" */

        if (is_virtual_target(target)) continue;

        struct stat st;
        if (stat(link_path, &st) != 0) continue; /* fd closed/raced away */
        if (st.st_size < min_size) continue;

        if (!cmdline_loaded) {
            read_cmdline(pid, cmdline, sizeof(cmdline));
            cmdline_loaded = 1;
        }

        hsed_entry_t e;
        memset(&e, 0, sizeof(e));
        e.pid = pid;
        e.fd = fdnum;
        fd_mode(pid, fdnum, e.mode);
        e.size = (long long)st.st_size;
        e.uid = owner_uid;
        hsed_strlcpy_local(e.user, owner_buf, sizeof(e.user));
        hsed_strlcpy_local(e.comm, comm, sizeof(e.comm));
        hsed_strlcpy_local(e.cmdline, cmdline, sizeof(e.cmdline));
        hsed_strlcpy_local(e.path, target, sizeof(e.path));
        e.inode = (unsigned long long)st.st_ino;
        e.dev_major = major(st.st_dev);
        e.dev_minor = minor(st.st_dev);
        e.mtime = st.st_mtime;

        if (list_push(out, &e) != 0) {
            closedir(d);
            return -1; /* out of memory */
        }
    }

    closedir(d);
    return 0;
}

static int cmp_by_size_desc(const void *a, const void *b) {
    const hsed_entry_t *ea = a, *eb = b;
    if (ea->size < eb->size) return 1;
    if (ea->size > eb->size) return -1;
    return 0;
}

int hsed_scan(hsed_list_t *out, long long min_size, pid_t only_pid) {
    hsed_list_init(out);

    if (only_pid > 0) {
        if (scan_one_pid(out, only_pid, min_size) != 0) return -1;
    } else {
        DIR *proc = opendir("/proc");
        if (!proc) return -1;

        struct dirent *de;
        while ((de = readdir(proc)) != NULL) {
            if (!is_all_digits(de->d_name)) continue;
            pid_t pid = (pid_t)atol(de->d_name);
            if (scan_one_pid(out, pid, min_size) != 0) {
                closedir(proc);
                return -1;
            }
        }
        closedir(proc);
    }

    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(hsed_entry_t), cmp_by_size_desc);
    }
    return 0;
}
