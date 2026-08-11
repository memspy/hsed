
#ifndef HSED_UTIL_H
#define HSED_UTIL_H

#include <stddef.h>


void hsed_log_init(int use_syslog);

void hsed_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hsed_log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

size_t hsed_strlcpy(char *dst, const char *src, size_t dstsize);

#endif 
