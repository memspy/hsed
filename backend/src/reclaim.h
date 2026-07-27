#ifndef HSED_RECLAIM_H
#define HSED_RECLAIM_H

#include <sys/types.h>
#include <stddef.h>


int hsed_truncate_fd(pid_t pid, int fd, long long *freed_out,
                      char *errbuf, size_t errlen);


int hsed_send_signal(pid_t pid, int sig, char *errbuf, size_t errlen);

#endif 
