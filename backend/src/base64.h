/*
 * base64.h — minimal base64 encoder. We use it to carry raw (possibly
 * binary, possibly containing NUL bytes or invalid UTF-8) captured write()
 * buffers inside a JSON string field without any escaping headaches.
 */
#ifndef HSED_BASE64_H
#define HSED_BASE64_H

#include <stddef.h>

/* Encodes `len` bytes from `data` into `out`, which must be at least
 * hsed_base64_encoded_len(len) bytes. NUL-terminates `out`. */
void hsed_base64_encode(const unsigned char *data, size_t len, char *out);

/* Required output buffer size (including the NUL terminator) for `len`
 * input bytes. */
size_t hsed_base64_encoded_len(size_t len);

#endif /* HSED_BASE64_H */
