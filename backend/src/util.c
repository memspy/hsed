#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

static int g_use_syslog = 0;

void hsed_log_init(int use_syslog) {
    g_use_syslog = use_syslog;
    if (g_use_syslog) {
        openlog("hsed", LOG_PID, LOG_DAEMON);
    }
}

void hsed_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(LOG_INFO, fmt, ap);
    } else {
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

void hsed_log_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(LOG_ERR, fmt, ap);
    } else {
        fputs("[error] ", stderr);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

size_t hsed_strlcpy(char *dst, const char *src, size_t dstsize) {
    size_t srclen = strlen(src);
    if (dstsize > 0) {
        size_t copy = srclen < dstsize - 1 ? srclen : dstsize - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}
