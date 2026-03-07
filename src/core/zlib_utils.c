/* src/core/zlib_utils.c */
#include "zlib_utils.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

/* compress src into *out (caller frees), returns compressed size or -1 */
int zlib_deflate(const uint8_t *src, size_t src_len,
                 uint8_t **out, size_t *out_len) {
    uLongf bound = compressBound((uLong)src_len);
    uint8_t *buf = malloc(bound);
    if (!buf) return -1;
    if (compress2(buf, &bound, src, (uLong)src_len, Z_BEST_SPEED) != Z_OK) {
        free(buf);
        return -1;
    }
    *out     = buf;
    *out_len = (size_t)bound;
    return 0;
}

/* decompress src into *out (caller frees), returns 0 or -1 */
int zlib_inflate(const uint8_t *src, size_t src_len,
                 uint8_t **out, size_t *out_len) {
    size_t guess = src_len * 4 + 256;
    uint8_t *buf = malloc(guess);
    if (!buf) return -1;

    z_stream zs = {0};
    zs.next_in   = (Bytef *)src;
    zs.avail_in  = (uInt)src_len;

    if (inflateInit(&zs) != Z_OK) { free(buf); return -1; }

    int ret;
    size_t total = 0;
    do {
        if (total >= guess) {
            guess *= 2;
            uint8_t *tmp = realloc(buf, guess);
            if (!tmp) { inflateEnd(&zs); free(buf); return -1; }
            buf = tmp;
        }
        zs.next_out  = buf + total;
        zs.avail_out = (uInt)(guess - total);
        ret = inflate(&zs, Z_NO_FLUSH);
        total = guess - zs.avail_out;
    } while (ret == Z_OK);

    inflateEnd(&zs);
    if (ret != Z_STREAM_END) { free(buf); return -1; }

    buf[total] = '\0'; /* null-terminate as convenience */
    *out     = buf;
    *out_len = total;
    return 0;
}
