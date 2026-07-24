/*
 * protocol.h — the wire format between hsed and its clients (the Python
 * TUI, or anything else). Deliberately simple:
 *
 *   client -> server: one command per line, whitespace-separated tokens
 *     SCAN [min_size] [pid]
 *     TRUNCATE <pid> <fd>
 *     HUP <pid>
 *     KILL <pid>
 *     STREAM <pid> <fd>
 *     PING
 *     QUIT
 *
 *   server -> client: one JSON object per line ("JSON Lines"). SCAN and
 *   STREAM produce a sequence of lines ending in a terminal "end"-type
 *   object; TRUNCATE/HUP/PING produce exactly one line.
 *
 * We hand-roll JSON output here (no library) because the schema is small
 * and fixed — this keeps the daemon dependency-free.
 */
#ifndef HSED_PROTOCOL_H
#define HSED_PROTOCOL_H

#include <stddef.h>
#include "proc_scan.h"

/* Escapes `src` for safe inclusion inside a JSON string literal (handles
 * ", \, and control characters). Truncates rather than overflowing if
 * `dst` is too small; always NUL-terminates. */
void hsed_json_escape(char *dst, size_t dstsize, const char *src);

/* Formats one scan entry as a JSON object (no trailing newline). */
void hsed_format_entry(const hsed_entry_t *e, char *out, size_t outsize);

/* Sends `json` followed by '\n' on `sockfd`, looping over send() until the
 * whole line is written or an error occurs. Returns 0 on success, -1 on
 * error (e.g. the peer disconnected). */
int hsed_send_line(int sockfd, const char *json);

/* printf-style convenience wrapper around hsed_send_line for short,
 * fixed-shape responses (results, errors, pings). */
int hsed_send_linef(int sockfd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* HSED_PROTOCOL_H */
