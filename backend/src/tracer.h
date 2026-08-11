#ifndef HSED_TRACER_H
#define HSED_TRACER_H

#include <sys/types.h>
#include <stddef.h>


typedef void (*hsed_attached_fn)(void *ctx);


typedef int (*hsed_poll_fn)(void *ctx);


typedef void (*hsed_write_fn)(void *ctx, pid_t tid, long ret,
                               const unsigned char *buf, size_t buflen,
                               int is_writev);

int hsed_trace_fd(pid_t pid, int fd, size_t max_capture,
                   hsed_attached_fn attached_cb, hsed_poll_fn poll_cb,
                   hsed_write_fn write_cb, void *ctx,
                   char *errbuf, size_t errlen);

#endif 
