#ifndef HSED_PROTOCOL_H
#define HSED_PROTOCOL_H

#include <stddef.h>
#include "proc_scan.h"

void hsed_json_escape(char *dst, size_t dstsize, const char *src);

void hsed_format_entry(const hsed_entry_t *e, char *out, size_t outsize);


int hsed_send_line(int sockfd, const char *json);


int hsed_send_linef(int sockfd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif 
