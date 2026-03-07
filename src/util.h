#ifndef HEP_UTIL_H
#define HEP_UTIL_H
#include "core/types.h"
#include <sys/stat.h>

typedef void (*file_walk_cb)(const char *rel_path, struct stat *st, void *user);

int  util_walk_files(const char *base_dir, const char *rel,
                     file_walk_cb cb, void *user);
int  util_write_file(const char *path, const uint8_t *data, size_t len);
int  config_get(const char *key, char *out, size_t out_len);
int  config_set(const char *key, const char *value);
int  stash_save(const char *message);
int  stash_list(void);
#endif
