#ifndef HEP_ZLIB_UTILS_H
#define HEP_ZLIB_UTILS_H
#include <stdint.h>
#include <stddef.h>
int zlib_deflate(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len);
int zlib_inflate(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len);
#endif
