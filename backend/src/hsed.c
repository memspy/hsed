
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "proc_scan.h"
#include "protocol.h"
#include "server.h"
#include "util.h"

#define DEFAULT_MAX_CAPTURE 200

static volatile sig_atomic_t g_shutdown = 0;

static void on_shutdown_signal(int signo) {
    (void)signo;
    g_shutdown = 1;
}

static const char *default_socket_path(void) {
    static char buf[256];
    if (geteuid() == 0) {
        return "/run/hsed.sock";
    }
    snprintf(buf, sizeof(buf), "/tmp/hsed-%u.sock", (unsigned)geteuid());
    return buf;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--foreground] [--scan-once] [--stats-once] [--socket PATH] "
        "[--pidfile PATH] [--max-capture N] [--min-size N] [--uid N] "
        "[--scan-threads N]\n", prog);
}

static void run_scan_once(long long min_size, uid_t uid_filter, int scan_threads) {
    hsed_list_t list;
    if (hsed_scan(&list, min_size, 0, uid_filter, scan_threads) != 0) {
        fprintf(stderr, "scan failed: %s\n", strerror(errno));
        exit(1);
    }
    char linebuf[HSED_PATH_MAX * 2 + HSED_CMDLINE_MAX * 2 + HSED_NAME_MAX * 4];
    long long total = 0;
    for (size_t i = 0; i < list.count; i++) {
        hsed_format_entry(&list.items[i], linebuf, sizeof(linebuf));
        printf("%s\n", linebuf);
        total += list.items[i].size;
    }
    printf("{\"type\":\"end\",\"count\":%zu,\"total_bytes\":%lld}\n", list.count, total);
    hsed_list_free(&list);
}

static void run_stats_once(long long min_size, uid_t uid_filter, int scan_threads) {
    hsed_stats_t stats;
    if (hsed_scan_stats(&stats, min_size, uid_filter, scan_threads) != 0) {
        fprintf(stderr, "stats scan failed: %s\n", strerror(errno));
        exit(1);
    }
    printf("{\"type\":\"stats\",\"count\":%zu,\"total_bytes\":%lld}\n",
           stats.count, stats.total_bytes);
}


static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);

    if (setsid() < 0) { perror("setsid"); exit(1); }

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0); 

    umask(0);
    if (chdir("/") != 0) { /* not fatal, but worth knowing about */ }

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
}

static void write_pidfile(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        hsed_log_err("could not write pidfile %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

int main(int argc, char **argv) {
    int foreground = 0;
    int scan_once = 0;
    int stats_once = 0;
    const char *socket_path = NULL;
    const char *pidfile = "/run/hsed.pid";
    size_t max_capture = DEFAULT_MAX_CAPTURE;
    long long min_size = 0;
    uid_t uid_filter = HSED_UID_ANY;
    int scan_threads = HSED_SCAN_THREADS_AUTO;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--scan-once") == 0) {
            scan_once = 1;
        } else if (strcmp(argv[i], "--stats-once") == 0) {
            stats_once = 1;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--pidfile") == 0 && i + 1 < argc) {
            pidfile = argv[++i];
        } else if (strcmp(argv[i], "--max-capture") == 0 && i + 1 < argc) {
            max_capture = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--min-size") == 0 && i + 1 < argc) {
            min_size = strtoll(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--uid") == 0 && i + 1 < argc) {
            uid_filter = (uid_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--scan-threads") == 0 && i + 1 < argc) {
            scan_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!socket_path) socket_path = default_socket_path();

    if (scan_once) {
        run_scan_once(min_size, uid_filter, scan_threads);
        return 0;
    }
    if (stats_once) {
        run_stats_once(min_size, uid_filter, scan_threads);
        return 0;
    }

    if (geteuid() != 0) {
        fprintf(stderr,
            "[!] Running without root: only your own processes are visible, "
            "and truncate/signal/stream actions will fail with EPERM on "
            "anything you don't own. Run with sudo for full coverage.\n");
    }

    
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_shutdown_signal);
    signal(SIGINT, on_shutdown_signal);

    if (!foreground) {
        daemonize();
        hsed_log_init(1); 
        write_pidfile(pidfile);
    } else {
        hsed_log_init(0); 
    }

    hsed_log("hsed starting (pid %d, socket %s, max_capture %zu, scan_threads %d)",
              (int)getpid(), socket_path, max_capture, scan_threads);

    int rc = hsed_server_run(socket_path, max_capture, scan_threads, &g_shutdown);

    if (!foreground) unlink(pidfile);

    return rc == 0 ? 0 : 1;
}
