/* src/index.c — staging area (.hep/index) */
#include "index.h"
#include "repo.h"
#include "core/types.h"
#include "core/sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Index file format (text, one entry per line):
 *   <mode> <sha1hex> <path>\n
 * Simple but sufficient for now.
 */

static int index_path(char *out, size_t out_len) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;
    snprintf(out, out_len, "%s/index", root);
    return HEP_OK;
}

int index_read(hep_index *idx) {
    char path[HEP_PATH_MAX];
    if (index_path(path, sizeof(path)) != HEP_OK) return HEP_ERR_NOREPO;

    idx->entries = NULL; idx->count = 0; idx->cap = 0;

    FILE *f = fopen(path, "r");
    if (!f) return HEP_OK; /* empty index is fine */

    char line[HEP_PATH_MAX + 60];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!*line) continue;

        if (idx->count >= idx->cap) {
            idx->cap = idx->cap ? idx->cap * 2 : 16;
            idx->entries = realloc(idx->entries,
                                   sizeof(hep_index_entry) * idx->cap);
        }
        hep_index_entry *e = &idx->entries[idx->count];
        char hex[41];
        if (sscanf(line, "%o %40s %4095s",
                   &e->mode, hex, e->path) != 3) continue;
        sha1_from_hex(hex, e->sha);
        idx->count++;
    }
    fclose(f);
    return HEP_OK;
}

int index_write(hep_index *idx) {
    char path[HEP_PATH_MAX];
    if (index_path(path, sizeof(path)) != HEP_OK) return HEP_ERR_NOREPO;

    FILE *f = fopen(path, "w");
    if (!f) return HEP_ERR_IO;
    for (size_t i = 0; i < idx->count; i++) {
        char hex[41];
        sha1_to_hex(idx->entries[i].sha, hex);
        fprintf(f, "%o %s %s\n",
                idx->entries[i].mode, hex, idx->entries[i].path);
    }
    fclose(f);
    return HEP_OK;
}

void index_free(hep_index *idx) {
    free(idx->entries);
    idx->entries = NULL; idx->count = 0; idx->cap = 0;
}

/* add or update entry in index */
int index_add_entry(hep_index *idx, const char *path,
                    const uint8_t sha[20], uint32_t mode) {
    /* update existing */
    for (size_t i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].path, path) == 0) {
            memcpy(idx->entries[i].sha, sha, HEP_SHA1_LEN);
            idx->entries[i].mode = mode;
            return HEP_OK;
        }
    }
    /* append */
    if (idx->count >= idx->cap) {
        idx->cap = idx->cap ? idx->cap * 2 : 16;
        idx->entries = realloc(idx->entries,
                               sizeof(hep_index_entry) * idx->cap);
    }
    hep_index_entry *e = &idx->entries[idx->count++];
    strncpy(e->path, path, HEP_PATH_MAX - 1);
    memcpy(e->sha, sha, HEP_SHA1_LEN);
    e->mode = mode;
    return HEP_OK;
}

/* remove entry from index */
int index_remove_entry(hep_index *idx, const char *path) {
    for (size_t i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].path, path) == 0) {
            memmove(&idx->entries[i], &idx->entries[i+1],
                    sizeof(hep_index_entry) * (idx->count - i - 1));
            idx->count--;
            return HEP_OK;
        }
    }
    return HEP_ERR_NOENT;
}

/* find entry by path */
hep_index_entry *index_find(hep_index *idx, const char *path) {
    for (size_t i = 0; i < idx->count; i++)
        if (strcmp(idx->entries[i].path, path) == 0)
            return &idx->entries[i];
    return NULL;
}
