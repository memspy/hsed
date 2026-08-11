#include "proc_scan.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define DELETED_SUFFIX " (deleted)"
#define DELETED_SUFFIX_LEN 10


#define HSED_MAX_AUTO_SCAN_THREADS 32

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
        if (arglen == 0) i = n; 
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
            long flags = strtol(line + 6, NULL, 8); 
            long acc = flags & 3; 
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


static int scan_one_pid(hsed_list_t *out, hsed_stats_t *stats, pid_t pid,
                         long long min_size, uid_t uid_filter) {
    char proc_path[32];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", (int)pid);
    struct stat pst;
    if (stat(proc_path, &pst) != 0) return 0; 

    uid_t owner_uid = pst.st_uid;
    if (uid_filter != HSED_UID_ANY && owner_uid != uid_filter) return 0;

    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%d/fd", (int)pid);
    DIR *d = opendir(fd_dir);
    if (!d) return 0; 

    char comm[HSED_NAME_MAX];
    int comm_loaded = 0;
    char cmdline[HSED_CMDLINE_MAX];
    int cmdline_loaded = 0;
    char owner_buf[HSED_NAME_MAX];
    int owner_loaded = 0;

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
        if (stat(link_path, &st) != 0) continue; 
        if (st.st_size < min_size) continue;

        if (stats) {
            stats->count++;
            stats->total_bytes += (long long)st.st_size;
            continue;
        }

        if (!comm_loaded) {
            char comm_path[64];
            snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)pid);
            read_first_line(comm_path, comm, sizeof(comm));
            if (comm[0] == '\0') hsed_strlcpy_local(comm, "?", sizeof(comm));
            comm_loaded = 1;
        }
        if (!cmdline_loaded) {
            read_cmdline(pid, cmdline, sizeof(cmdline));
            cmdline_loaded = 1;
        }
        if (!owner_loaded) {
            owner_name(owner_uid, owner_buf, sizeof(owner_buf));
            owner_loaded = 1;
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


typedef struct {
    pid_t *pids;
    size_t count;
} pid_list_t;

static int gather_pids(pid_list_t *out) {
    out->pids = NULL;
    out->count = 0;
    size_t cap = 0;

    DIR *proc = opendir("/proc");
    if (!proc) return -1;

    struct dirent *de;
    while ((de = readdir(proc)) != NULL) {
        if (!is_all_digits(de->d_name)) continue;
        if (out->count == cap) {
            size_t newcap = cap == 0 ? 256 : cap * 2;
            pid_t *grown = realloc(out->pids, newcap * sizeof(pid_t));
            if (!grown) {
                closedir(proc);
                free(out->pids);
                out->pids = NULL;
                return -1;
            }
            out->pids = grown;
            cap = newcap;
        }
        out->pids[out->count++] = (pid_t)atol(de->d_name);
    }
    closedir(proc);
    return 0;
}

static int pick_thread_count(int requested, size_t npids) {
    long n;
    if (requested > 0) {
        n = requested;
    } else {
        n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > HSED_MAX_AUTO_SCAN_THREADS) n = HSED_MAX_AUTO_SCAN_THREADS;
    }
    if (npids == 0) return 1;
    if ((size_t)n > npids) n = (long)npids;
    if (n < 1) n = 1;
    return (int)n;
}

typedef struct {
    pid_t *pids;
    size_t count;
    long long min_size;
    uid_t uid_filter;
    int use_stats;

    hsed_list_t list;   
    hsed_stats_t stats; 
    int failed;
} scan_worker_t;

static void *scan_worker_fn(void *arg) {
    scan_worker_t *w = (scan_worker_t *)arg;
    if (w->use_stats) {
        w->stats.count = 0;
        w->stats.total_bytes = 0;
        for (size_t i = 0; i < w->count; i++) {
            if (scan_one_pid(NULL, &w->stats, w->pids[i], w->min_size, w->uid_filter) != 0) {
                w->failed = 1;
                break;
            }
        }
    } else {
        hsed_list_init(&w->list);
        for (size_t i = 0; i < w->count; i++) {
            if (scan_one_pid(&w->list, NULL, w->pids[i], w->min_size, w->uid_filter) != 0) {
                w->failed = 1;
                break;
            }
        }
    }
    return NULL;
}


static int run_parallel_scan(pid_list_t *pl, long long min_size, uid_t uid_filter,
                              int num_threads, int use_stats,
                              hsed_list_t *out_list, hsed_stats_t *out_stats) {
    int nthreads = pick_thread_count(num_threads, pl->count);

    scan_worker_t *workers = calloc((size_t)nthreads, sizeof(scan_worker_t));
    pthread_t *tids = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!workers || !tids) {
        free(workers);
        free(tids);
        return -1;
    }

    size_t base = pl->count / (size_t)nthreads;
    size_t rem = pl->count % (size_t)nthreads;
    size_t offset = 0;
    for (int i = 0; i < nthreads; i++) {
        size_t chunk = base + ((size_t)i < rem ? 1 : 0);
        workers[i].pids = pl->pids + offset;
        workers[i].count = chunk;
        workers[i].min_size = min_size;
        workers[i].uid_filter = uid_filter;
        workers[i].use_stats = use_stats;
        offset += chunk;
    }

    for (int i = 1; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, scan_worker_fn, &workers[i]) != 0) {
            tids[i] = 0; /* wasn't actually spawned; run it inline below */
            scan_worker_fn(&workers[i]);
        }
    }
    scan_worker_fn(&workers[0]);
    for (int i = 1; i < nthreads; i++) {
        if (tids[i] != 0) pthread_join(tids[i], NULL);
    }

    int failed = 0;
    for (int i = 0; i < nthreads; i++) {
        if (workers[i].failed) failed = 1;
    }

    if (!failed) {
        if (use_stats) {
            out_stats->count = 0;
            out_stats->total_bytes = 0;
            for (int i = 0; i < nthreads; i++) {
                out_stats->count += workers[i].stats.count;
                out_stats->total_bytes += workers[i].stats.total_bytes;
            }
        } else {
            hsed_list_init(out_list);
            for (int i = 0; i < nthreads && !failed; i++) {
                for (size_t j = 0; j < workers[i].list.count; j++) {
                    if (list_push(out_list, &workers[i].list.items[j]) != 0) {
                        failed = 1;
                        break;
                    }
                }
            }
            if (failed) hsed_list_free(out_list);
        }
    }

    for (int i = 0; i < nthreads; i++) {
        if (!use_stats) hsed_list_free(&workers[i].list);
    }
    free(workers);
    free(tids);
    return failed ? -1 : 0;
}

int hsed_scan(hsed_list_t *out, long long min_size, pid_t only_pid,
              uid_t uid_filter, int num_threads) {
    if (only_pid > 0) {
        hsed_list_init(out);
        if (scan_one_pid(out, NULL, only_pid, min_size, uid_filter) != 0) return -1;
        if (out->count > 1) qsort(out->items, out->count, sizeof(hsed_entry_t), cmp_by_size_desc);
        return 0;
    }

    pid_list_t pl;
    if (gather_pids(&pl) != 0) return -1;

    int rc = run_parallel_scan(&pl, min_size, uid_filter, num_threads, 0, out, NULL);
    free(pl.pids);
    if (rc != 0) return -1;

    if (out->count > 1) {
        qsort(out->items, out->count, sizeof(hsed_entry_t), cmp_by_size_desc);
    }
    return 0;
}

int hsed_scan_stats(hsed_stats_t *out, long long min_size, uid_t uid_filter,
                     int num_threads) {
    pid_list_t pl;
    if (gather_pids(&pl) != 0) return -1;

    int rc = run_parallel_scan(&pl, min_size, uid_filter, num_threads, 1, NULL, out);
    free(pl.pids);
    return rc;
}
