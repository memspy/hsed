/*
 * util.h — small shared helpers: logging (stderr or syslog, depending on
 * whether we've daemonized) and a couple of bounded string helpers.
 */
#ifndef HSED_UTIL_H
#define HSED_UTIL_H

#include <stddef.h>

/* Call once after deciding whether we're running as a daemon (syslog) or
 * in the foreground (stderr). */
void hsed_log_init(int use_syslog);

void hsed_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hsed_log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Bounded copy that always NUL-terminates dst (size must be > 0).
 * Returns the length of src (like strlcpy), so callers can detect
 * truncation by comparing against dstsize. */
size_t hsed_strlcpy(char *dst, const char *src, size_t dstsize);

#endif /* HSED_UTIL_H */
