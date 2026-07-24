/*
 * hsed.c — Hidden Space Explorer Daemon (binary name: hsedd).
 *
 * Finds files that were deleted (unlinked) but are still held open by a
 * process fd — the reason `df -h` can show a full disk while `du -sh`
 * finds nothing to account for it. Runs as a background service accepting
 * commands over a Unix domain socket, or on-demand for a single scan.
 *
 * The interactive command a user actually types is `hsed` (the Python TUI,
 * see ../../hidden_space_explorer/) — it talks to this daemon over the
 * socket. This binary is the privileged backend, not something you'd run
 * by hand day-to-day.
 *
 * Usage:
 *   hsedd                       run as a daemon (forks into the background)
 *   hsedd --foreground          run as a daemon, but stay attached (systemd
 *                              Type=simple, or convenient for debugging)
 *   hsedd --scan-once           print one JSON-lines scan to stdout and exit
 *                              (no socket, no daemon — for cron/scripts)
 *   hsedd --socket <path>       override the Unix socket path
 *   hsedd --pidfile <path>      override the pidfile path (daemon mode only)
 *   hsedd --max-capture <n>     bytes of write() payload STREAM previews
 *                              per event (default 200)
 *   hsedd --min-size <n>        with --scan-once, skip entries smaller than
 *                              n bytes
 */
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
        "Usage: %s [--foreground] [--scan-once] [--socket PATH] "
        "[--pidfile PATH] [--max-capture N]\n", prog);
}

static void run_scan_once(long long min_size) {
    hsed_list_t list;
    if (hsed_scan(&list, min_size, 0) != 0) {
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

/* Classic double-fork daemonization: detach from the controlling terminal,
 * become a session leader, and make sure we can never reacquire one. */
static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0); /* first parent exits */

    if (setsid() < 0) { perror("setsid"); exit(1); }

    pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0); /* second parent exits */

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
    const char *socket_path = NULL;
    const char *pidfile = "/run/hsed.pid";
    size_t max_capture = DEFAULT_MAX_CAPTURE;
    long long min_size = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--scan-once") == 0) {
            scan_once = 1;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--pidfile") == 0 && i + 1 < argc) {
            pidfile = argv[++i];
        } else if (strcmp(argv[i], "--max-capture") == 0 && i + 1 < argc) {
            max_capture = (size_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--min-size") == 0 && i + 1 < argc) {
            min_size = strtoll(argv[++i], NULL, 10);
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
        run_scan_once(min_size);
        return 0;
    }

    if (geteuid() != 0) {
        fprintf(stderr,
            "[!] Running without root: only your own processes are visible, "
            "and truncate/signal/stream actions will fail with EPERM on "
            "anything you don't own. Run with sudo for full coverage.\n");
    }

    /* SIGPIPE would otherwise kill us the instant a client disconnects
     * mid-write; we handle short writes/EPIPE explicitly in protocol.c. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_shutdown_signal);
    signal(SIGINT, on_shutdown_signal);

    if (!foreground) {
        daemonize();
        hsed_log_init(1); /* syslog once detached */
        write_pidfile(pidfile);
    } else {
        hsed_log_init(0); /* stderr while attached */
    }

    hsed_log("hsed starting (pid %d, socket %s, max_capture %zu)",
              (int)getpid(), socket_path, max_capture);

    int rc = hsed_server_run(socket_path, max_capture, &g_shutdown);

    if (!foreground) unlink(pidfile);

    return rc == 0 ? 0 : 1;
}
