/* src/util.c */
#include "util.h"
#include "repo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

/* ── file walking ─────────────────────────────────────────────────── */

static int should_ignore(const char *name) {
    return strcmp(name, ".hep") == 0 ||
           strcmp(name, ".") == 0 ||
           strcmp(name, "..") == 0;
}

/* walk directory recursively, call cb for each regular file */
int util_walk_files(const char *base_dir, const char *rel,
                    file_walk_cb cb, void *user) {
    char full[HEP_PATH_MAX];
    if (*rel)
        snprintf(full, sizeof(full), "%s/%s", base_dir, rel);
    else
        snprintf(full, sizeof(full), "%s", base_dir);

    DIR *d = opendir(full);
    if (!d) return HEP_ERR;

    struct dirent *de;
    while ((de = readdir(d))) {
        if (should_ignore(de->d_name)) continue;

        char child_rel[HEP_PATH_MAX];
        if (*rel)
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, de->d_name);
        else
            snprintf(child_rel, sizeof(child_rel), "%s", de->d_name);

        char child_full[HEP_PATH_MAX];
        snprintf(child_full, sizeof(child_full), "%s/%s", full, de->d_name);

        struct stat st;
        if (stat(child_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            util_walk_files(base_dir, child_rel, cb, user);
        } else if (S_ISREG(st.st_mode)) {
            cb(child_rel, &st, user);
        }
    }
    closedir(d);
    return HEP_OK;
}

/* ── config ───────────────────────────────────────────────────────── */

int config_get(const char *key, char *out, size_t out_len) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char cfg_path[HEP_PATH_MAX];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config", root);

    FILE *f = fopen(cfg_path, "r");
    if (!f) return HEP_ERR_NOENT;

    /* look for key=value or key = value */
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line; while (*k == '\t' || *k == ' ') k++;
        char *end = eq - 1; while (end > k && (*end == ' ' || *end == '\t')) *end-- = '\0';
        if (strcmp(k, key) != 0) continue;
        char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
        strncpy(out, v, out_len - 1);
        fclose(f);
        return HEP_OK;
    }
    fclose(f);
    return HEP_ERR_NOENT;
}

int config_set(const char *key, const char *value) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char cfg_path[HEP_PATH_MAX];
    snprintf(cfg_path, sizeof(cfg_path), "%s/config", root);

    /* read existing */
    char *lines[512]; size_t n = 0;
    FILE *f = fopen(cfg_path, "r");
    int found = 0;
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f) && n < 511) {
            lines[n] = strdup(line);
            char tmp[512]; strncpy(tmp, line, sizeof(tmp));
            tmp[strcspn(tmp,"\n")] = '\0';
            char *eq = strchr(tmp, '=');
            if (eq) {
                *eq = '\0';
                char *k = tmp; while (*k==' '||*k=='\t') k++;
                char *e = eq-1; while (e>k&&(*e==' '||*e=='\t')) *e--='\0';
                if (strcmp(k, key) == 0) {
                    free(lines[n]);
                    char buf[512];
                    snprintf(buf, sizeof(buf), "\t%s = %s\n", key, value);
                    lines[n] = strdup(buf);
                    found = 1;
                }
            }
            n++;
        }
        fclose(f);
    }
    if (!found) {
        char buf[512];
        snprintf(buf, sizeof(buf), "\t%s = %s\n", key, value);
        lines[n++] = strdup(buf);
    }

    f = fopen(cfg_path, "w");
    if (!f) { for(size_t i=0;i<n;i++) free(lines[i]); return HEP_ERR_IO; }
    for (size_t i = 0; i < n; i++) { fputs(lines[i], f); free(lines[i]); }
    fclose(f);
    return HEP_OK;
}

/* ── stash ────────────────────────────────────────────────────────── */

int stash_save(const char *message) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    /* for now: write a stash entry marker (full impl needs diff engine) */
    char stash_path[HEP_PATH_MAX];
    snprintf(stash_path, sizeof(stash_path), "%s/stash/stash-%ld", root, time(NULL));

    FILE *f = fopen(stash_path, "w");
    if (!f) return HEP_ERR_IO;
    fprintf(f, "message: %s\ntime: %ld\n", message ? message : "WIP", time(NULL));
    fclose(f);
    return HEP_OK;
}

int stash_list(void) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char stash_dir[HEP_PATH_MAX];
    snprintf(stash_dir, sizeof(stash_dir), "%s/stash", root);

    DIR *d = opendir(stash_dir);
    if (!d) { printf("stash: nothing stashed bro\n"); return HEP_OK; }

    int i = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        printf("stash@{%d}: %s\n", i++, de->d_name);
    }
    if (i == 0) printf("stash: nothing stashed bro\n");
    closedir(d);
    return HEP_OK;
}

/* ── misc ─────────────────────────────────────────────────────────── */

/* write file from buf */
int util_write_file(const char *path, const uint8_t *data, size_t len) {
    /* ensure parent dirs exist */
    char dir[HEP_PATH_MAX]; strncpy(dir, path, sizeof(dir));
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = '\0';
        /* mkdir -p */
        for (char *p = dir+1; *p; p++) {
            if (*p == '/') {
                *p = '\0'; mkdir(dir, 0755); *p = '/';
            }
        }
        mkdir(dir, 0755);
    }
    FILE *f = fopen(path, "wb");
    if (!f) return HEP_ERR_IO;
    fwrite(data, 1, len, f);
    fclose(f);
    return HEP_OK;
}
