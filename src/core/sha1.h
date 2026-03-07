#ifndef HEP_SHA1_H
#define HEP_SHA1_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buf[64];
} sha1_ctx;

void sha1_init(sha1_ctx *ctx);
void sha1_update(sha1_ctx *ctx, const uint8_t *data, size_t len);
void sha1_final(sha1_ctx *ctx, uint8_t digest[20]);
void sha1_buf(const uint8_t *data, size_t len, uint8_t digest[20]);
void sha1_to_hex(const uint8_t sha[20], char hex[41]);
int  sha1_from_hex(const char *hex, uint8_t sha[20]);
#endif
