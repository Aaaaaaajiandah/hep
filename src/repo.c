/* src/repo.c */
#include "repo.h"
#include "core/types.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* walk up from cwd until we find a .hep directory */
int repo_find_root(char *out, size_t out_len) {
    char cwd[HEP_PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return HEP_ERR;

    char try[HEP_PATH_MAX];
    char *p = cwd + strlen(cwd);
    while (1) {
        snprintf(try, sizeof(try), "%s/.hep", cwd);
        struct stat st;
        if (stat(try, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(out, out_len, "%s", try);
            return HEP_OK;
        }
        /* go one level up */
        p = strrchr(cwd, '/');
        if (!p || p == cwd) break;
        *p = '\0';
    }
    return HEP_ERR_NOREPO;
}

/* resolve HEAD to hex sha (or empty string if unborn) */
int repo_head_sha(char hex[41]) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char head_path[HEP_PATH_MAX];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", root);

    FILE *f = fopen(head_path, "r");
    if (!f) return HEP_ERR_IO;
    char line[256]; fgets(line, sizeof(line), f); fclose(f);

    /* symbolic ref? */
    if (strncmp(line, "ref: ", 5) == 0) {
        char ref[HEP_PATH_MAX];
        sscanf(line + 5, "%s", ref);
        char ref_path[HEP_PATH_MAX];
        snprintf(ref_path, sizeof(ref_path), "%s/%s", root, ref);
        FILE *rf = fopen(ref_path, "r");
        if (!rf) { hex[0] = '\0'; return HEP_OK; } /* unborn branch */
        fgets(line, sizeof(line), rf); fclose(rf);
        line[strcspn(line,"\n")] = '\0';
        strncpy(hex, line, 41); hex[40] = '\0';
        return HEP_OK;
    }
    /* direct sha */
    line[strcspn(line,"\n")] = '\0';
    strncpy(hex, line, 41); hex[40] = '\0';
    return HEP_OK;
}

/* get current branch name (from HEAD) */
int repo_current_branch(char *out, size_t out_len) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char head_path[HEP_PATH_MAX];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", root);

    FILE *f = fopen(head_path, "r");
    if (!f) return HEP_ERR_IO;
    char line[256]; fgets(line, sizeof(line), f); fclose(f);
    line[strcspn(line,"\n")] = '\0';

    if (strncmp(line, "ref: refs/heads/", 16) == 0) {
        strncpy(out, line + 16, out_len);
        return HEP_OK;
    }
    strncpy(out, "(detached)", out_len);
    return HEP_OK;
}

/* write ref: e.g. refs/heads/main -> sha hex */
int repo_write_ref(const char *ref, const char *hex) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char ref_path[HEP_PATH_MAX];
    snprintf(ref_path, sizeof(ref_path), "%s/%s", root, ref);

    /* ensure parent dir exists */
    char dir[HEP_PATH_MAX]; strncpy(dir, ref_path, sizeof(dir));
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; mkdir(dir, 0755); }

    FILE *f = fopen(ref_path, "w");
    if (!f) return HEP_ERR_IO;
    fprintf(f, "%s\n", hex);
    fclose(f);
    return HEP_OK;
}

/* read ref, returns hex or HEP_ERR_NOENT */
int repo_read_ref(const char *ref, char hex[41]) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;

    char ref_path[HEP_PATH_MAX];
    snprintf(ref_path, sizeof(ref_path), "%s/%s", root, ref);

    FILE *f = fopen(ref_path, "r");
    if (!f) return HEP_ERR_NOENT;
    char line[64]; fgets(line, sizeof(line), f); fclose(f);
    line[strcspn(line,"\n")] = '\0';
    strncpy(hex, line, 41); hex[40] = '\0';
    return HEP_OK;
}

/* update HEAD branch to point to hex */
int repo_update_head(const char *hex) {
    char branch[256];
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return HEP_ERR_NOREPO;
    if (repo_current_branch(branch, sizeof(branch)) != HEP_OK) return HEP_ERR;

    char ref[HEP_PATH_MAX];
    snprintf(ref, sizeof(ref), "refs/heads/%s", branch);
    return repo_write_ref(ref, hex);
}

int cmd_init(int argc, char **argv) {
    (void)argc; (void)argv;
    struct stat st;
    if (stat(".hep", &st) == 0 && S_ISDIR(st.st_mode)) {
        printf("Folder already exists, bro\n");
        return 1;
    }
    const char *dirs[] = {
        ".hep", ".hep/objects", ".hep/refs",
        ".hep/refs/heads", ".hep/refs/tags", ".hep/stash", NULL
    };
    for (int i = 0; dirs[i]; i++) {
        if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) {
            perror(dirs[i]); return 1;
        }
    }
    FILE *head = fopen(".hep/HEAD", "w");
    if (!head) { perror(".hep/HEAD"); return 1; }
    fprintf(head, "ref: refs/heads/main\n");
    fclose(head);

    FILE *cfg = fopen(".hep/config", "w");
    if (cfg) { fprintf(cfg, "[core]\n\tbare = false\n"); fclose(cfg); }

    printf("Initialized empty hep repository in .hep/ 😎\n");
    return 0;
}
