/* src/core/blob.c */
#include "blob.h"
#include "odb.h"
#include <stdio.h>
#include <stdlib.h>

/* hash a file and write it to the odb, returns hex sha in out_hex */
int blob_from_file(const char *path, char out_hex[41]) {
    FILE *f = fopen(path, "rb");
    if (!f) return HEP_ERR_NOENT;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return HEP_ERR; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    int r = odb_write(OBJ_BLOB, buf, (size_t)sz, out_hex);
    free(buf);
    return r;
}

/* hash raw bytes as a blob, return hex sha */
int blob_from_buf(const uint8_t *data, size_t len, char out_hex[41]) {
    return odb_write(OBJ_BLOB, data, len, out_hex);
}

/* read blob content (caller frees out->data) */
int blob_read(const char *hex, hep_buf *out) {
    hep_obj_type t;
    int r = odb_read(hex, &t, out);
    if (r != HEP_OK) return r;
    if (t != OBJ_BLOB) return HEP_ERR_CORRUPT;
    return HEP_OK;
}
