#include "base64.h"

static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t hsed_base64_encoded_len(size_t len) {
    return ((len + 2) / 3) * 4 + 1;
}

void hsed_base64_encode(const unsigned char *data, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out[o++] = B64_TABLE[(v >> 18) & 0x3F];
        out[o++] = B64_TABLE[(v >> 12) & 0x3F];
        out[o++] = B64_TABLE[(v >> 6) & 0x3F];
        out[o++] = B64_TABLE[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        unsigned int v = data[i] << 16;
        out[o++] = B64_TABLE[(v >> 18) & 0x3F];
        out[o++] = B64_TABLE[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8);
        out[o++] = B64_TABLE[(v >> 18) & 0x3F];
        out[o++] = B64_TABLE[(v >> 12) & 0x3F];
        out[o++] = B64_TABLE[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
}
