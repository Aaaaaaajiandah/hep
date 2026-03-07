#ifndef HEP_REPO_H
#define HEP_REPO_H
#include <stddef.h>
#include "core/types.h"
int cmd_init(int argc, char **argv);
int repo_find_root(char *out, size_t out_len);
int repo_head_sha(char hex[41]);
int repo_current_branch(char *out, size_t out_len);
int repo_write_ref(const char *ref, const char *hex);
int repo_read_ref(const char *ref, char hex[41]);
int repo_update_head(const char *hex);
#endif
