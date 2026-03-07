#ifndef HEP_BLOB_H
#define HEP_BLOB_H
#include "types.h"
int blob_from_file(const char *path, char out_hex[41]);
int blob_from_buf(const uint8_t *data, size_t len, char out_hex[41]);
int blob_read(const char *hex, hep_buf *out);
#endif
