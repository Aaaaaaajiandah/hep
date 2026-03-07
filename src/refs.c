/* src/refs.c */
#include "refs.h"
#include "repo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int refs_list_branches(char ***out_names, size_t *out_count) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char heads_dir[HEP_PATH_MAX];
    snprintf(heads_dir, sizeof(heads_dir), "%s/refs/heads", root);

    DIR *d = opendir(heads_dir);
    if (!d) { *out_names = NULL; *out_count = 0; return HEP_OK; }

    size_t cap = 8, count = 0;
    char **names = malloc(sizeof(char*) * cap);

    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (count >= cap) { cap *= 2; names = realloc(names, sizeof(char*)*cap); }
        names[count++] = strdup(de->d_name);
    }
    closedir(d);
    *out_names = names; *out_count = count;
    return HEP_OK;
}

void refs_free_list(char **names, size_t count) {
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

int refs_create_branch(const char *name, const char *hex) {
    char ref[HEP_PATH_MAX];
    snprintf(ref, sizeof(ref), "refs/heads/%s", name);
    return repo_write_ref(ref, hex);
}

int refs_delete_branch(const char *name) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;
    char path[HEP_PATH_MAX];
    snprintf(path, sizeof(path), "%s/refs/heads/%s", root, name);
    return remove(path) == 0 ? HEP_OK : HEP_ERR_NOENT;
}

int refs_branch_sha(const char *name, char hex[41]) {
    char ref[HEP_PATH_MAX];
    snprintf(ref, sizeof(ref), "refs/heads/%s", name);
    return repo_read_ref(ref, hex);
}

int refs_create_tag(const char *name, const char *hex) {
    char ref[HEP_PATH_MAX];
    snprintf(ref, sizeof(ref), "refs/tags/%s", name);
    return repo_write_ref(ref, hex);
}

int refs_list_tags(char ***out_names, size_t *out_count) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char tags_dir[HEP_PATH_MAX];
    snprintf(tags_dir, sizeof(tags_dir), "%s/refs/tags", root);

    DIR *d = opendir(tags_dir);
    if (!d) { *out_names = NULL; *out_count = 0; return HEP_OK; }

    size_t cap = 8, count = 0;
    char **names = malloc(sizeof(char*) * cap);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (count >= cap) { cap *= 2; names = realloc(names, sizeof(char*)*cap); }
        names[count++] = strdup(de->d_name);
    }
    closedir(d);
    *out_names = names; *out_count = count;
    return HEP_OK;
}
