#ifndef HEP_REFS_H
#define HEP_REFS_H
#include "core/types.h"
#include <stddef.h>
int  refs_list_branches(char ***out_names, size_t *out_count);
void refs_free_list(char **names, size_t count);
int  refs_create_branch(const char *name, const char *hex);
int  refs_delete_branch(const char *name);
int  refs_branch_sha(const char *name, char hex[41]);
int  refs_create_tag(const char *name, const char *hex);
int  refs_list_tags(char ***out_names, size_t *out_count);
#endif
