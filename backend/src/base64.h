
#ifndef HSED_BASE64_H
#define HSED_BASE64_H

#include <stddef.h>


void hsed_base64_encode(const unsigned char *data, size_t len, char *out);


size_t hsed_base64_encoded_len(size_t len);

#endif 
