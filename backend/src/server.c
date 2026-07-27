#include "server.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "base64.h"
#include "protocol.h"
#include "proc_scan.h"
#include "reclaim.h"
#include "tracer.h"
#include "util.h"

typedef struct {
    int sockfd;
    size_t max_capture;
} conn_args_t;


static ssize_t recv_line(int fd, char *buf, size_t bufsize) {
    size_t i = 0;
    for (;;) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return -1;              /* peer closed */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') {
            buf[i] = '\0';
            return (ssize_t)i;
        }
        if (i + 1 < bufsize) buf[i++] = c;   /* silently drop overflow chars */
    }
}

static void handle_scan(int sockfd, long long min_size, pid_t only_pid) {
    hsed_list_t list;
    if (hsed_scan(&list, min_size, only_pid) != 0) {
        hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"scan failed: %s\"}", strerror(errno));
        return;
    }
    long long total = 0;
    char linebuf[HSED_PATH_MAX * 2 + HSED_CMDLINE_MAX * 2 + HSED_NAME_MAX * 4];
    for (size_t i = 0; i < list.count; i++) {
        hsed_format_entry(&list.items[i], linebuf, sizeof(linebuf));
        if (hsed_send_line(sockfd, linebuf) != 0) { hsed_list_free(&list); return; }
        total += list.items[i].size;
    }
    hsed_send_linef(sockfd, "{\"type\":\"end\",\"count\":%zu,\"total_bytes\":%lld}", list.count, total);
    hsed_list_free(&list);
}

static void handle_truncate(int sockfd, pid_t pid, int fd) {
    long long freed = 0;
    char err[256];
    if (hsed_truncate_fd(pid, fd, &freed, err, sizeof(err)) != 0) {
        char esc[512];
        hsed_json_escape(esc, sizeof(esc), err);
        hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"%s\"}", esc);
        return;
    }
    hsed_send_linef(sockfd, "{\"type\":\"result\",\"ok\":true,\"freed\":%lld}", freed);
}

static void handle_hup(int sockfd, pid_t pid) {
    char err[256];
    if (hsed_send_signal(pid, SIGHUP, err, sizeof(err)) != 0) {
        char esc[512];
        hsed_json_escape(esc, sizeof(esc), err);
        hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"%s\"}", esc);
        return;
    }
    hsed_send_linef(sockfd, "{\"type\":\"result\",\"ok\":true}");
}


static void handle_kill(int sockfd, pid_t pid) {
    char err[256];
    if (hsed_send_signal(pid, SIGKILL, err, sizeof(err)) != 0) {
        char esc[512];
        hsed_json_escape(esc, sizeof(esc), err);
        hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"%s\"}", esc);
        return;
    }
    hsed_send_linef(sockfd, "{\"type\":\"result\",\"ok\":true}");
}

typedef struct {
    int sockfd;
    pid_t pid;
    int fd;
} stream_ctx_t;

static int stream_poll_cb(void *arg) {
    stream_ctx_t *sc = (stream_ctx_t *)arg;
    char buf[256];
    ssize_t n = recv(sc->sockfd, buf, sizeof(buf), MSG_DONTWAIT);
    if (n == 0) return 0;                          /* client closed the connection */
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : 0;
    if (memmem(buf, (size_t)n, "STOP", 4) != NULL) return 0; /* explicit stop request */
    return 1;
}

static void stream_write_cb(void *arg, pid_t tid, long ret,
                             const unsigned char *buf, size_t buflen, int is_writev) {
    stream_ctx_t *sc = (stream_ctx_t *)arg;
    char stackbuf[1024];
    size_t need = hsed_base64_encoded_len(buflen);
    char *b64 = need <= sizeof(stackbuf) ? stackbuf : malloc(need);
    if (!b64) return;
    hsed_base64_encode(buf, buflen, b64);
    hsed_send_linef(sc->sockfd,
        "{\"type\":\"write\",\"tid\":%d,\"ret\":%ld,\"captured\":%zu,\"is_writev\":%s,\"data_b64\":\"%s\"}",
        (int)tid, ret, buflen, is_writev ? "true" : "false", b64);
    if (b64 != stackbuf) free(b64);
}

static void stream_attached_cb(void *arg) {
    stream_ctx_t *sc = (stream_ctx_t *)arg;
    hsed_send_linef(sc->sockfd, "{\"type\":\"attached\",\"pid\":%d,\"fd\":%d}", (int)sc->pid, sc->fd);
}

static void handle_stream(int sockfd, pid_t pid, int fd, size_t max_capture) {
    stream_ctx_t sc = { .sockfd = sockfd, .pid = pid, .fd = fd };
    char err[256] = { 0 };
    int rc = hsed_trace_fd(pid, fd, max_capture, stream_attached_cb, stream_poll_cb,
                            stream_write_cb, &sc, err, sizeof(err));
    if (rc != 0) {
        char esc[512];
        hsed_json_escape(esc, sizeof(esc), err);
        hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"%s\"}", esc);
        return;
    }
    hsed_send_linef(sockfd, "{\"type\":\"stream_end\"}");
}

static void *handle_connection(void *arg) {
    conn_args_t *cargs = (conn_args_t *)arg;
    int sockfd = cargs->sockfd;
    size_t max_capture = cargs->max_capture;
    free(cargs);

    char line[512];
    for (;;) {
        ssize_t n = recv_line(sockfd, line, sizeof(line));
        if (n < 0) break; /* disconnected */

        char *save = NULL;
        char *cmd = strtok_r(line, " \t\r\n", &save);
        if (!cmd) continue;

        if (strcasecmp(cmd, "PING") == 0) {
            hsed_send_linef(sockfd, "{\"type\":\"pong\"}");
        } else if (strcasecmp(cmd, "QUIT") == 0) {
            break;
        } else if (strcasecmp(cmd, "SCAN") == 0) {
            char *a1 = strtok_r(NULL, " \t\r\n", &save);
            char *a2 = strtok_r(NULL, " \t\r\n", &save);
            long long min_size = a1 ? atoll(a1) : 0;
            pid_t only_pid = a2 ? (pid_t)atol(a2) : 0;
            handle_scan(sockfd, min_size, only_pid);
        } else if (strcasecmp(cmd, "TRUNCATE") == 0) {
            char *a1 = strtok_r(NULL, " \t\r\n", &save);
            char *a2 = strtok_r(NULL, " \t\r\n", &save);
            if (!a1 || !a2) {
                hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"usage: TRUNCATE <pid> <fd>\"}");
                continue;
            }
            handle_truncate(sockfd, (pid_t)atol(a1), atoi(a2));
        } else if (strcasecmp(cmd, "HUP") == 0) {
            char *a1 = strtok_r(NULL, " \t\r\n", &save);
            if (!a1) {
                hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"usage: HUP <pid>\"}");
                continue;
            }
            handle_hup(sockfd, (pid_t)atol(a1));
        } else if (strcasecmp(cmd, "KILL") == 0) {
            char *a1 = strtok_r(NULL, " \t\r\n", &save);
            if (!a1) {
                hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"usage: KILL <pid>\"}");
                continue;
            }
            handle_kill(sockfd, (pid_t)atol(a1));
        } else if (strcasecmp(cmd, "STREAM") == 0) {
            char *a1 = strtok_r(NULL, " \t\r\n", &save);
            char *a2 = strtok_r(NULL, " \t\r\n", &save);
            if (!a1 || !a2) {
                hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"usage: STREAM <pid> <fd>\"}");
                continue;
            }
            handle_stream(sockfd, (pid_t)atol(a1), atoi(a2), max_capture);
        } else {
            hsed_send_linef(sockfd, "{\"type\":\"error\",\"message\":\"unknown command\"}");
        }
    }

    close(sockfd);
    return NULL;
}

int hsed_server_run(const char *socket_path, size_t max_capture,
                     volatile sig_atomic_t *shutdown_flag) {
    unlink(socket_path); /* remove a stale socket left by a previous run */

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        hsed_log_err("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    hsed_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        hsed_log_err("bind(%s) failed: %s", socket_path, strerror(errno));
        close(listen_fd);
        return -1;
    }
    /* Root-only by default. Widen this (e.g. chmod 0660 + chgrp to an ops
     * group) if the whole team should be able to drive the daemon. */
    chmod(socket_path, 0600);

    if (listen(listen_fd, 16) != 0) {
        hsed_log_err("listen() failed: %s", strerror(errno));
        close(listen_fd);
        unlink(socket_path);
        return -1;
    }

    hsed_log("listening on %s", socket_path);

    while (!*shutdown_flag) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 500); /* 500ms so we notice shutdown_flag promptly */
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        conn_args_t *cargs = malloc(sizeof(*cargs));
        if (!cargs) { close(client_fd); continue; }
        cargs->sockfd = client_fd;
        cargs->max_capture = max_capture;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_connection, cargs) != 0) {
            hsed_log_err("pthread_create failed: %s", strerror(errno));
            close(client_fd);
            free(cargs);
            continue;
        }
        pthread_detach(tid);
    }

    close(listen_fd);
    unlink(socket_path);
    hsed_log("shut down");
    return 0;
}
