#ifndef HEP_ODB_H
#define HEP_ODB_H
#include "types.h"
int odb_write(hep_obj_type type, const uint8_t *data, size_t len, char out_hex[41]);
int odb_read(const char *hex, hep_obj_type *out_type, hep_buf *out_buf);
int odb_exists(const char *hex);
#endif
