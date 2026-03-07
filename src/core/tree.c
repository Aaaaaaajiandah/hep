/* src/core/tree.c */
#include "tree.h"
#include "odb.h"
#include "sha1.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Tree binary format (like git):
 *   for each entry:
 *     "<mode> <name>\0<20-byte-sha>"
 */

int tree_write(hep_tree *tree, char out_hex[41]) {
    /* compute total size */
    size_t total = 0;
    for (size_t i = 0; i < tree->count; i++) {
        char mode_str[16];
        snprintf(mode_str, sizeof(mode_str), "%o", tree->entries[i].mode);
        total += strlen(mode_str) + 1 + strlen(tree->entries[i].name) + 1 + HEP_SHA1_LEN;
    }

    uint8_t *buf = malloc(total);
    if (!buf) return HEP_ERR;

    size_t pos = 0;
    for (size_t i = 0; i < tree->count; i++) {
        hep_tree_entry *e = &tree->entries[i];
        char mode_str[16];
        int mlen = snprintf(mode_str, sizeof(mode_str), "%o", e->mode);
        memcpy(buf + pos, mode_str, (size_t)mlen); pos += (size_t)mlen;
        buf[pos++] = ' ';
        size_t nlen = strlen(e->name);
        memcpy(buf + pos, e->name, nlen); pos += nlen;
        buf[pos++] = '\0';
        memcpy(buf + pos, e->sha, HEP_SHA1_LEN); pos += HEP_SHA1_LEN;
    }

    int r = odb_write(OBJ_TREE, buf, total, out_hex);
    free(buf);
    return r;
}

int tree_read(const char *hex, hep_tree *out) {
    hep_obj_type t;
    hep_buf buf;
    int r = odb_read(hex, &t, &buf);
    if (r != HEP_OK) return r;
    if (t != OBJ_TREE) { free(buf.data); return HEP_ERR_CORRUPT; }

    /* count entries */
    size_t count = 0, pos = 0;
    while (pos < buf.len) {
        /* skip mode+space */
        while (pos < buf.len && buf.data[pos] != ' ') pos++;
        pos++; /* skip space */
        /* skip name+null */
        while (pos < buf.len && buf.data[pos] != '\0') pos++;
        pos++; /* skip null */
        pos += HEP_SHA1_LEN;
        count++;
    }

    out->entries = malloc(sizeof(hep_tree_entry) * count);
    if (!out->entries) { free(buf.data); return HEP_ERR; }
    out->count = count;

    pos = 0;
    for (size_t i = 0; i < count; i++) {
        hep_tree_entry *e = &out->entries[i];
        /* mode */
        char mode_str[16]; size_t mpos = 0;
        while (pos < buf.len && buf.data[pos] != ' ')
            mode_str[mpos++] = (char)buf.data[pos++];
        mode_str[mpos] = '\0'; pos++; /* skip space */
        e->mode = (uint32_t)strtoul(mode_str, NULL, 8);
        /* name */
        size_t npos = 0;
        while (pos < buf.len && buf.data[pos] != '\0')
            e->name[npos++] = (char)buf.data[pos++];
        e->name[npos] = '\0'; pos++; /* skip null */
        /* sha */
        memcpy(e->sha, buf.data + pos, HEP_SHA1_LEN);
        pos += HEP_SHA1_LEN;
    }

    free(buf.data);
    return HEP_OK;
}

void tree_free(hep_tree *tree) {
    free(tree->entries);
    tree->entries = NULL;
    tree->count   = 0;
}
