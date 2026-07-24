#include "protocol.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

void hsed_json_escape(char *dst, size_t dstsize, const char *src) {
    size_t o = 0;
    if (dstsize == 0) return;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 2 < dstsize; p++) {
        switch (*p) {
            case '"':  if (o + 2 < dstsize) { dst[o++] = '\\'; dst[o++] = '"'; } break;
            case '\\': if (o + 2 < dstsize) { dst[o++] = '\\'; dst[o++] = '\\'; } break;
            case '\n': if (o + 2 < dstsize) { dst[o++] = '\\'; dst[o++] = 'n'; } break;
            case '\r': if (o + 2 < dstsize) { dst[o++] = '\\'; dst[o++] = 'r'; } break;
            case '\t': if (o + 2 < dstsize) { dst[o++] = '\\'; dst[o++] = 't'; } break;
            default:
                if (*p < 0x20) {
                    if (o + 6 < dstsize) {
                        int n = snprintf(dst + o, dstsize - o, "\\u%04x", *p);
                        if (n > 0) o += (size_t)n;
                    }
                } else {
                    dst[o++] = (char)*p;
                }
        }
    }
    dst[o < dstsize ? o : dstsize - 1] = '\0';
}

void hsed_format_entry(const hsed_entry_t *e, char *out, size_t outsize) {
    char user_esc[HSED_NAME_MAX * 2];
    char comm_esc[HSED_NAME_MAX * 2];
    char cmdline_esc[HSED_CMDLINE_MAX * 2];
    char path_esc[HSED_PATH_MAX * 2];

    hsed_json_escape(user_esc, sizeof(user_esc), e->user);
    hsed_json_escape(comm_esc, sizeof(comm_esc), e->comm);
    hsed_json_escape(cmdline_esc, sizeof(cmdline_esc), e->cmdline);
    hsed_json_escape(path_esc, sizeof(path_esc), e->path);

    snprintf(out, outsize,
             "{\"type\":\"entry\",\"pid\":%d,\"fd\":%d,\"mode\":\"%s\","
             "\"size\":%lld,\"uid\":%u,\"user\":\"%s\",\"comm\":\"%s\","
             "\"cmdline\":\"%s\",\"path\":\"%s\",\"inode\":%llu,"
             "\"dev_major\":%u,\"dev_minor\":%u,\"mtime\":%lld}",
             (int)e->pid, e->fd, e->mode, e->size, (unsigned)e->uid,
             user_esc, comm_esc, cmdline_esc, path_esc, e->inode,
             e->dev_major, e->dev_minor, (long long)e->mtime);
}

int hsed_send_line(int sockfd, const char *json) {
    size_t len = strlen(json);
    /* +1 for the newline delimiter clients read lines on. */
    for (size_t sent = 0; sent < len + 1; ) {
        const char *chunk = sent < len ? json + sent : "\n";
        size_t chunklen = sent < len ? len - sent : 1;
        ssize_t n = send(sockfd, chunk, chunklen, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int hsed_send_linef(int sockfd, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return hsed_send_line(sockfd, buf);
}
