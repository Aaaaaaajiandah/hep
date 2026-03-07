/* src/core/odb.c — object database (loose objects under .hep/objects/) */
#include "odb.h"
#include "sha1.h"
#include "zlib_utils.h"
#include "types.h"
#include "../repo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

static const char *type_name(hep_obj_type t) {
    switch(t) {
        case OBJ_BLOB:   return "blob";
        case OBJ_TREE:   return "tree";
        case OBJ_COMMIT: return "commit";
        case OBJ_TAG:    return "tag";
        default:         return "unknown";
    }
}

static hep_obj_type type_from_name(const char *name) {
    if (!strcmp(name,"blob"))   return OBJ_BLOB;
    if (!strcmp(name,"tree"))   return OBJ_TREE;
    if (!strcmp(name,"commit")) return OBJ_COMMIT;
    if (!strcmp(name,"tag"))    return OBJ_TAG;
    return (hep_obj_type)0;
}

/*
 * Object format on disk:
 *   zlib( "<type> <size>\0<raw bytes>" )
 * stored at .hep/objects/<xx>/<remaining 38 hex chars>
 */

/* write object, return sha1 hex in out_hex[41] */
int odb_write(hep_obj_type type, const uint8_t *data, size_t len,
              char out_hex[41]) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    /* build header */
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_name(type), len);

    /* header + null + data */
    size_t raw_len = (size_t)hlen + 1 + len;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return HEP_ERR;
    memcpy(raw, header, (size_t)hlen);
    raw[hlen] = '\0';
    memcpy(raw + hlen + 1, data, len);

    /* sha1 of raw */
    hep_sha1 sha;
    sha1_buf(raw, raw_len, sha);
    sha1_to_hex(sha, out_hex);

    /* path: .hep/objects/xx/yyy... */
    char obj_dir[HEP_PATH_MAX], obj_path[HEP_PATH_MAX];
    snprintf(obj_dir,  sizeof(obj_dir),  "%s/objects/%.2s", root, out_hex);
    snprintf(obj_path, sizeof(obj_path), "%s/%s",           obj_dir, out_hex+2);

    /* already exists? */
    struct stat st;
    if (stat(obj_path, &st) == 0) { free(raw); return HEP_OK; }

    /* compress */
    uint8_t *comp; size_t comp_len;
    if (zlib_deflate(raw, raw_len, &comp, &comp_len) != 0) {
        free(raw); return HEP_ERR;
    }
    free(raw);

    /* mkdir xx/ */
    if (mkdir(obj_dir, 0755) != 0 && errno != EEXIST) {
        free(comp); return HEP_ERR_IO;
    }

    /* write to temp then rename (atomic) */
    char tmp_path[HEP_PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", obj_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) { free(comp); return HEP_ERR_IO; }
    fwrite(comp, 1, comp_len, f);
    fclose(f);
    free(comp);
    rename(tmp_path, obj_path);
    return HEP_OK;
}

/* read object by hex hash, fills type + buf (caller frees buf.data) */
int odb_read(const char *hex, hep_obj_type *out_type, hep_buf *out_buf) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    if (strlen(hex) < 4) return HEP_ERR;

    char obj_path[HEP_PATH_MAX];
    snprintf(obj_path, sizeof(obj_path), "%s/objects/%.2s/%s", root, hex, hex+2);

    FILE *f = fopen(obj_path, "rb");
    if (!f) return HEP_ERR_NOENT;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    uint8_t *comp = malloc((size_t)fsz);
    if (!comp) { fclose(f); return HEP_ERR; }
    fread(comp, 1, (size_t)fsz, f);
    fclose(f);

    uint8_t *raw; size_t raw_len;
    if (zlib_inflate(comp, (size_t)fsz, &raw, &raw_len) != 0) {
        free(comp); return HEP_ERR_CORRUPT;
    }
    free(comp);

    /* parse header: "<type> <size>\0" */
    char *null_pos = memchr(raw, '\0', raw_len);
    if (!null_pos) { free(raw); return HEP_ERR_CORRUPT; }

    char type_str[16] = {0};
    sscanf((char*)raw, "%15s", type_str);
    *out_type = type_from_name(type_str);

    size_t hdr_len = (size_t)(null_pos - (char*)raw) + 1;
    out_buf->len  = raw_len - hdr_len;
    out_buf->data = malloc(out_buf->len + 1);
    if (!out_buf->data) { free(raw); return HEP_ERR; }
    memcpy(out_buf->data, raw + hdr_len, out_buf->len);
    out_buf->data[out_buf->len] = '\0';
    free(raw);
    return HEP_OK;
}

/* check object exists */
int odb_exists(const char *hex) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return 0;
    char obj_path[HEP_PATH_MAX];
    snprintf(obj_path, sizeof(obj_path), "%s/objects/%.2s/%s", root, hex, hex+2);
    struct stat st;
    return stat(obj_path, &st) == 0;
}
