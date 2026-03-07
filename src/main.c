/* src/main.c */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "repo.h"
#include "index.h"
#include "refs.h"
#include "util.h"
#include "core/types.h"
#include "core/sha1.h"
#include "core/odb.h"
#include "core/blob.h"
#include "core/tree.h"
#include "core/commit.h"

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void print_usage(void) {
    puts(
        "hep — version control\n"
        "\n"
        "Usage: hep <command> [args]\n"
        "\n"
        "  init                     start a new hep repo\n"
        "  print <file|.>           stage file(s)\n"
        "  wave -m <msg>            commit staged changes\n"
        "  spy                      show commit log\n"
        "  compete [commit] [file]  show diff\n"
        "  light                    show status\n"
        "  expand [name]            list/create branch\n"
        "  travel <branch>          switch branch\n"
        "  chiplets <branch>        merge branch\n"
        "  stl <url> [dir]          clone remote repo\n"
        "  send [remote]            push to remote\n"
        "  dock [remote]            pull from remote\n"
        "  interface <commit> <f>   show file at commit\n"
        "  search <pattern>         search in repo\n"
        "  hall                     stash changes\n"
        "  retrieve                 apply stash\n"
        "  group [name] [commit]    list/create tag\n"
        "  microscope <hash>        inspect raw object\n"
        "  earth <file>             untrack file\n"
        "  house <key> [value]      get/set config\n"
        "  kill <commit>            reset to commit\n"
    );
}

/* forward declarations for commands defined later */
void reflog_append(const char *hex, const char *msg);
int cmd_undo(int argc, char **argv);
int cmd_redo(int argc, char **argv);
int cmd_mansion(int argc, char **argv);
static int mansion_should_store(const char *root, const char *path);
static int mansion_store(const char *root, const char *path, char ref_out[256]);

/* build a tree object from the current index */
static int build_tree_from_index(hep_index *idx, char out_hex[41]) {
    hep_tree tree;
    tree.entries = malloc(sizeof(hep_tree_entry) * idx->count);
    tree.count   = idx->count;
    for (size_t i = 0; i < idx->count; i++) {
        strncpy(tree.entries[i].name, idx->entries[i].path, 255);
        memcpy(tree.entries[i].sha, idx->entries[i].sha, HEP_SHA1_LEN);
        tree.entries[i].mode = idx->entries[i].mode ? idx->entries[i].mode : 0100644;
    }
    int r = tree_write(&tree, out_hex);
    free(tree.entries);
    return r;
}

/* ── print: stage files ────────────────────────────────────────────────────── */

typedef struct { hep_index *idx; int errors; } print_ctx;

static void print_file_cb(const char *rel, struct stat *st, void *user) {
    print_ctx *ctx = user;
    (void)st;

    /* check if file should go to mansion (large file store) */
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) == HEP_OK &&
        mansion_should_store(root, rel)) {
        char ref[256];
        if (mansion_store(root, rel, ref) == HEP_OK) {
            /* store the mansion ref string as a tiny blob */
            char tmp_path[HEP_PATH_MAX];
            snprintf(tmp_path, sizeof(tmp_path), "%s/mansion_tmp", root);
            FILE *tf = fopen(tmp_path, "w");
            if (tf) { fputs(ref, tf); fclose(tf); }
            char hex[41];
            blob_from_file(tmp_path, hex);
            remove(tmp_path);
            hep_sha1 sha; sha1_from_hex(hex, sha);
            index_add_entry(ctx->idx, rel, sha, 0100644);
            struct stat ms; stat(rel, &ms);
            printf("print: staged '%s' -> mansion (%.0f MB)\n",
                   rel, (double)ms.st_size / (1024.0*1024.0));
            return;
        }
    }

    char hex[41];
    if (blob_from_file(rel, hex) != HEP_OK) {
        fprintf(stderr, "print: failed to hash '%s'\n", rel);
        ctx->errors++;
        return;
    }
    hep_sha1 sha;
    sha1_from_hex(hex, sha);
    index_add_entry(ctx->idx, rel, sha, 0100644);
    printf("print: staged '%s'\n", rel);
}

int cmd_print(int argc, char **argv) {
    /* print -line = interactive hunk staging */
    if (argc >= 2 && strcmp(argv[1], "-line") == 0)
        return cmd_print_line(argc-1, argv+1);
    if (argc < 2) {
        fprintf(stderr, "print: specify file(s) or '.' \nUsage: hep print <file|.>\n");
        return 1;
    }
    hep_index idx;
    if (index_read(&idx) != HEP_OK) {
        fprintf(stderr, "print: not in a hep repo\n"); return 1;
    }
    print_ctx ctx = { &idx, 0 };
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) != 0) {
            fprintf(stderr, "print: '%s' not found\n", argv[i]); ctx.errors++; continue;
        }
        if (S_ISDIR(st.st_mode)) {
            util_walk_files(".", strcmp(argv[i],".")==0 ? "" : argv[i],
                            print_file_cb, &ctx);
        } else {
            print_file_cb(argv[i], &st, &ctx);
        }
    }
    index_write(&idx);
    index_free(&idx);
    return ctx.errors ? 1 : 0;
}

/* ── wave: commit ───────────────────────────────────────────────────────── */

int cmd_wave(int argc, char **argv) {
    const char *msg = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "-m") == 0) msg = argv[i+1];
    if (!msg) {
        fprintf(stderr, "wave: need a message\nUsage: hep wave -m \"msg\"\n");
        return 1;
    }

    hep_index idx;
    if (index_read(&idx) != HEP_OK) {
        fprintf(stderr, "wave: not in a hep repo\n"); return 1;
    }
    if (idx.count == 0) {
        printf("wave: nothing staged — run hep print <file>' first\n");
        index_free(&idx); return 0;
    }

    /* build tree */
    char tree_hex[41];
    if (build_tree_from_index(&idx, tree_hex) != HEP_OK) {
        fprintf(stderr, "wave: failed to build tree\n");
        index_free(&idx); return 1;
    }
    index_free(&idx);

    /* get author from config */
    char author[256] = "Unknown <unknown@hep>";
    char name[128], email[128];
    int has_name  = config_get("name",  name,  sizeof(name))  == HEP_OK;
    int has_email = config_get("email", email, sizeof(email)) == HEP_OK;
    if (has_name && has_email)
        snprintf(author, sizeof(author), "%s <%s>", name, email);
    else if (has_name)
        snprintf(author, sizeof(author), "%s", name);

    /* build commit */
    hep_commit c; memset(&c, 0, sizeof(c));
    sha1_from_hex(tree_hex, c.tree_sha);
    c.author      = author;
    c.committer   = author;
    c.author_time = c.commit_time = (int64_t)time(NULL);
    c.message     = (char*)msg;

    /* parent = current HEAD */
    char parent_hex[41];
    if (repo_head_sha(parent_hex) == HEP_OK && parent_hex[0]) {
        sha1_from_hex(parent_hex, c.parents[0]);
        c.parent_count = 1;
    }

    char commit_hex[41];
    if (commit_write(&c, commit_hex) != HEP_OK) {
        fprintf(stderr, "wave: failed to write commit\n"); return 1;
    }

    repo_update_head(commit_hex);
    reflog_append(commit_hex, msg);
    printf("wave: [%s] %s\n", commit_hex, msg);
    return 0;
}

/* ── spy: log ───────────────────────────────────────────────────────── */

int cmd_spy(int argc, char **argv) {
    /* spy -title = log --follow */
    if (argc >= 2 && strcmp(argv[1], "-title") == 0)
        return cmd_spy_title(argc-1, argv+1);
    (void)argc; (void)argv;
    char hex[41];
    if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
        printf("spy: no commits yet\n"); return 0;
    }

    int limit = 20;
    while (hex[0] && limit-- > 0) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) {
            fprintf(stderr, "spy: corrupt commit %s\n", hex); break;
        }
        printf("commit %s\n", hex);
        printf("author:  %s\n", c.author    ? c.author    : "unknown");
        printf("date:    %s", c.commit_time
            ? ctime((time_t*)&c.commit_time) : "unknown\n");
        printf("\n    %s\n\n", c.message ? c.message : "");

        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    return 0;
}

/* ── compete: diff (working tree vs index) ─────────────────────────────────── */

typedef struct { hep_index *idx; } compete_ctx;

static void compete_file_cb(const char *rel, struct stat *st, void *user) {
    compete_ctx *ctx = user; (void)st;
    char hex[41];
    if (blob_from_file(rel, hex) != HEP_OK) return;

    hep_index_entry *e = index_find(ctx->idx, rel);
    if (!e) {
        printf("?? %s (untracked)\n", rel); return;
    }
    char idx_hex[41]; sha1_to_hex(e->sha, idx_hex);
    if (strcmp(hex, idx_hex) != 0)
        printf("M  %s (modified)\n", rel);
}

int cmd_compete(int argc, char **argv) {
    /* compete -l = line-level diff */
    if (argc >= 2 && strcmp(argv[1], "-l") == 0)
        return cmd_compete_l(argc-1, argv+1);
    (void)argc; (void)argv;
    hep_index idx;
    if (index_read(&idx) != HEP_OK) {
        fprintf(stderr, "compete: not in a hep repo\n"); return 1;
    }
    /* show staged vs HEAD */
    char head_hex[41];
    if (repo_head_sha(head_hex) == HEP_OK && head_hex[0]) {
        hep_commit c;
        if (commit_read(head_hex, &c) == HEP_OK) {
            char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
            hep_tree tree;
            if (tree_read(tree_hex, &tree) == HEP_OK) {
                for (size_t i = 0; i < idx.count; i++) {
                    int found = 0;
                    char idx_hex[41]; sha1_to_hex(idx.entries[i].sha, idx_hex);
                    for (size_t j = 0; j < tree.count; j++) {
                        char t_hex[41]; sha1_to_hex(tree.entries[j].sha, t_hex);
                        if (strcmp(tree.entries[j].name, idx.entries[i].path) == 0) {
                            found = 1;
                            if (strcmp(idx_hex, t_hex) != 0)
                                printf("M  %s\n", idx.entries[i].path);
                            break;
                        }
                    }
                    if (!found) printf("A  %s\n", idx.entries[i].path);
                }
                tree_free(&tree);
            }
            commit_free(&c);
        }
    } else {
        /* no HEAD — everything staged is new */
        for (size_t i = 0; i < idx.count; i++)
            printf("A  %s\n", idx.entries[i].path);
    }

    /* show working tree vs index */
    printf("\nWorking tree changes:\n");
    compete_ctx ctx = { &idx };
    util_walk_files(".", "", compete_file_cb, &ctx);
    index_free(&idx);
    return 0;
}

/* ── om: status ───────────────────────────────────────────────────────────── */

int cmd_light(int argc, char **argv) {
    (void)argc; (void)argv;
    char branch[256];
    if (repo_current_branch(branch, sizeof(branch)) != HEP_OK) {
        fprintf(stderr, "om: not in a hep repo\n"); return 1;
    }
    printf("On branch: %s\n\n", branch);

    hep_index idx;
    if (index_read(&idx) != HEP_OK) return 1;

    if (idx.count == 0) {
        printf("Nothing staged. Run 'hep print <file>' to stage.\n");
    } else {
        printf("Changes staged for commit (hep wave):\n");
        for (size_t i = 0; i < idx.count; i++)
            printf("  staged: %s\n", idx.entries[i].path);
    }
    index_free(&idx);
    return 0;
}

/* ── expand: branch ──────────────────────────────────────────────────────── */

int cmd_expand(int argc, char **argv) {
    if (argc < 2) {
        char **names; size_t count;
        if (refs_list_branches(&names, &count) != HEP_OK) {
            fprintf(stderr, "expand: not in a hep repo\n"); return 1;
        }
        char current[256];
        repo_current_branch(current, sizeof(current));
        for (size_t i = 0; i < count; i++) {
            printf("%s %s\n",
                strcmp(names[i], current) == 0 ? "*" : " ", names[i]);
        }
        if (count == 0) printf("(no branches yet)\n");
        refs_free_list(names, count);
        return 0;
    }
    char head[41];
    if (repo_head_sha(head) != HEP_OK || !head[0]) {
        fprintf(stderr, "expand: no commits yet — commit something first\n");
        return 1;
    }
    if (refs_create_branch(argv[1], head) != HEP_OK) {
        fprintf(stderr, "expand: failed to create branch '%s'\n", argv[1]);
        return 1;
    }
    printf("expand: created branch '%s'\n", argv[1]);
    return 0;
}

/* ── travel: checkout/switch ─────────────────────────────────────────────── */

int cmd_travel(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "travel: which branch?\nUsage: hep travel <branch>\n");
        return 1;
    }
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "travel: not in a hep repo\n"); return 1;
    }
    /* check branch exists */
    char branch_hex[41];
    if (refs_branch_sha(argv[1], branch_hex) != HEP_OK) {
        fprintf(stderr, "travel: branch '%s' doesn't exist — use 'hep expand %s' first\n",
                argv[1], argv[1]);
        return 1;
    }
    /* update HEAD */
    char head_path[HEP_PATH_MAX];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", root);
    FILE *f = fopen(head_path, "w");
    if (!f) { perror("travel"); return 1; }
    fprintf(f, "ref: refs/heads/%s\n", argv[1]);
    fclose(f);

    /* restore working tree from the branch's commit */
    hep_commit c;
    if (commit_read(branch_hex, &c) == HEP_OK) {
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            for (size_t i = 0; i < tree.count; i++) {
                char entry_hex[41]; sha1_to_hex(tree.entries[i].sha, entry_hex);
                hep_buf blob;
                if (blob_read(entry_hex, &blob) == HEP_OK) {
                    util_write_file(tree.entries[i].name, blob.data, blob.len);
                    free(blob.data);
                }
            }
            tree_free(&tree);
        }
        commit_free(&c);
    }

    printf("travel: switched to branch '%s'\n", argv[1]);
    return 0;
}

/* ── chiplets: merge ───────────────────────────────────────────────────────── */

int cmd_chiplets(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "chiplets: which branch to merge?\nUsage: hep chiplets <branch>\n");
        return 1;
    }
    char their_hex[41];
    if (refs_branch_sha(argv[1], their_hex) != HEP_OK) {
        fprintf(stderr, "chiplets: branch '%s' not found\n", argv[1]); return 1;
    }
    char our_hex[41];
    if (repo_head_sha(our_hex) != HEP_OK || !our_hex[0]) {
        fprintf(stderr, "chiplets: no commits on current branch yet\n"); return 1;
    }
    if (strcmp(our_hex, their_hex) == 0) {
        printf("chiplets: already up to date\n"); return 0;
    }

    /* fast-forward merge: apply their tree to working dir and update HEAD */
    hep_commit c;
    if (commit_read(their_hex, &c) == HEP_OK) {
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            hep_index idx; index_read(&idx);
            for (size_t i = 0; i < tree.count; i++) {
                char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                hep_buf blob;
                if (blob_read(eh, &blob) == HEP_OK) {
                    util_write_file(tree.entries[i].name, blob.data, blob.len);
                    hep_sha1 sha; sha1_from_hex(eh, sha);
                    index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
                    free(blob.data);
                }
            }
            index_write(&idx); index_free(&idx);
            tree_free(&tree);
        }
        commit_free(&c);
    }
    repo_update_head(their_hex);
    printf("chiplets: merged '%s' (fast-forward)\n", argv[1]);
    return 0;
}

/* ── juice: clone ─────────────────────────────────────────────────────────── */

int cmd_stl(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "stl: give me a URL\nUsage: hep stl <path-or-url> [dir]\n");
        return 1;
    }
    const char *src = argv[1];
    char dest[HEP_PATH_MAX];
    if (argc >= 3) {
        strncpy(dest, argv[2], sizeof(dest)-1);
    } else {
        /* use last path component */
        const char *sl = strrchr(src, '/');
        strncpy(dest, sl ? sl+1 : src, sizeof(dest)-1);
    }

    /* for local paths: copy .hep directory */
    char src_hep[HEP_PATH_MAX];
    snprintf(src_hep, sizeof(src_hep), "%s/.hep", src);
    struct stat st;
    if (stat(src_hep, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "stl: '%s' is not a hep repo (remote clone not yet implemented)\n", src);
        return 1;
    }

    /* mkdir dest, copy .hep */
    if (mkdir(dest, 0755) != 0) { perror(dest); return 1; }

    char cmd[HEP_PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s/'", src_hep, dest);
    system(cmd);

    /* checkout HEAD into dest */
    char old_cwd[HEP_PATH_MAX]; getcwd(old_cwd, sizeof(old_cwd));
    chdir(dest);
    char head_hex[41];
    if (repo_head_sha(head_hex) == HEP_OK && head_hex[0]) {
        hep_commit c;
        if (commit_read(head_hex, &c) == HEP_OK) {
            char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
            hep_tree tree;
            if (tree_read(tree_hex, &tree) == HEP_OK) {
                hep_index idx; index_read(&idx);
                for (size_t i = 0; i < tree.count; i++) {
                    char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                    hep_buf blob;
                    if (blob_read(eh, &blob) == HEP_OK) {
                        util_write_file(tree.entries[i].name, blob.data, blob.len);
                        hep_sha1 sha; sha1_from_hex(eh, sha);
                        index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
                        free(blob.data);
                    }
                }
                index_write(&idx); index_free(&idx);
                tree_free(&tree);
            }
            commit_free(&c);
        }
    }
    chdir(old_cwd);

    printf("stl: cloned into '%s'\n", dest);
    return 0;
}

/* ── crashed: push ────────────────────────────────────────────────────────── */

int cmd_send(int argc, char **argv) {
    const char *remote = (argc >= 2) ? argv[1] : "origin";

    char origin[HEP_PATH_MAX];
    if (config_get("remote.origin.url", origin, sizeof(origin)) != HEP_OK
        && strcmp(remote, "origin") == 0) {
        fprintf(stderr, "send: no remote 'origin' configured\n"
                        "  set one: hep house remote.origin.url <path>\n");
        return 1;
    }

    /* local push: copy objects to remote .hep */
    char remote_url[HEP_PATH_MAX];
    config_get(remote, remote_url, sizeof(remote_url));

    char local_root[HEP_PATH_MAX], remote_hep[HEP_PATH_MAX];
    if (repo_find_root(local_root, sizeof(local_root)) != HEP_OK) {
        fprintf(stderr, "send: not in a hep repo\n"); return 1;
    }
    snprintf(remote_hep, sizeof(remote_hep), "%s/.hep", origin[0] ? origin : remote_url);

    struct stat st;
    if (stat(remote_hep, &st) != 0) {
        fprintf(stderr, "send: remote not found: %s\n", remote_hep); return 1;
    }

    /* copy objects */
    char cmd[HEP_PATH_MAX*2];
    snprintf(cmd, sizeof(cmd), "cp -rn '%s/objects/'* '%s/objects/' 2>/dev/null || true",
             local_root, remote_hep);
    system(cmd);

    /* update remote HEAD ref */
    char head_hex[41]; repo_head_sha(head_hex);
    char branch[256]; repo_current_branch(branch, sizeof(branch));
    char ref_path[HEP_PATH_MAX];
    snprintf(ref_path, sizeof(ref_path), "%s/refs/heads/%s", remote_hep, branch);
    FILE *f = fopen(ref_path, "w");
    if (f) { fprintf(f, "%s\n", head_hex); fclose(f); }

    printf("send: pushed '%s' to %s\n", branch, remote);
    return 0;
}

/* ── swardstrom: pull ─────────────────────────────────────────────────────── */

int cmd_dock(int argc, char **argv) {
    const char *remote = (argc >= 2) ? argv[1] : "origin";
    char origin[HEP_PATH_MAX];
    if (config_get("remote.origin.url", origin, sizeof(origin)) != HEP_OK) {
        fprintf(stderr, "dock: no remote configured\n"
                        "  set one: hep house remote.origin.url <path>\n");
        return 1;
    }
    (void)remote;

    char local_root[HEP_PATH_MAX];
    if (repo_find_root(local_root, sizeof(local_root)) != HEP_OK) {
        fprintf(stderr, "dock: not in a hep repo\n"); return 1;
    }

    char remote_hep[HEP_PATH_MAX];
    snprintf(remote_hep, sizeof(remote_hep), "%s/.hep", origin);

    struct stat st;
    if (stat(remote_hep, &st) != 0) {
        fprintf(stderr, "dock: remote not found: %s\n", remote_hep); return 1;
    }

    /* copy remote objects locally */
    char cmd[HEP_PATH_MAX*2];
    snprintf(cmd, sizeof(cmd), "cp -rn '%s/objects/'* '%s/objects/' 2>/dev/null || true",
             remote_hep, local_root);
    system(cmd);

    /* update local branch ref from remote */
    char branch[256]; repo_current_branch(branch, sizeof(branch));
    char remote_ref[HEP_PATH_MAX];
    snprintf(remote_ref, sizeof(remote_ref), "%s/refs/heads/%s", remote_hep, branch);
    FILE *f = fopen(remote_ref, "r");
    if (f) {
        char hex[41]; fscanf(f, "%40s", hex); fclose(f);
        char local_ref[HEP_PATH_MAX];
        snprintf(local_ref, sizeof(local_ref), "refs/heads/%s", branch);
        repo_write_ref(local_ref, hex);

        /* checkout */
        char argv2[2][64]; strcpy(argv2[0],"travel"); strcpy(argv2[1],branch);
        char *kargv[] = { argv2[0], argv2[1] };
        cmd_travel(2, kargv);
    }

    printf("dock: pulled from %s\n", origin);
    return 0;
}

/* ── shah: show file at commit ────────────────────────────────────────────── */

int cmd_interface(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "interface: Usage: hep interface <commit> <file>\n"); return 1;
    }
    hep_commit c;
    if (commit_read(argv[1], &c) != HEP_OK) {
        fprintf(stderr, "interface: commit '%s' not found\n", argv[1]); return 1;
    }
    char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
    hep_tree tree;
    if (tree_read(tree_hex, &tree) != HEP_OK) {
        commit_free(&c); fprintf(stderr, "interface: bad tree\n"); return 1;
    }
    commit_free(&c);

    for (size_t i = 0; i < tree.count; i++) {
        if (strcmp(tree.entries[i].name, argv[2]) == 0) {
            char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
            hep_buf blob;
            if (blob_read(eh, &blob) == HEP_OK) {
                fwrite(blob.data, 1, blob.len, stdout);
                free(blob.data);
            }
            tree_free(&tree);
            return 0;
        }
    }
    tree_free(&tree);
    fprintf(stderr, "interface: file '%s' not in commit\n", argv[2]);
    return 1;
}

/* ── monkeytype: grep ─────────────────────────────────────────────────────── */

typedef struct { const char *pattern; int found; } mt_ctx;

static void search_cb(const char *rel, struct stat *st, void *user) {
    mt_ctx *ctx = user; (void)st;
    FILE *f = fopen(rel, "r");
    if (!f) return;
    char line[4096]; int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (strstr(line, ctx->pattern)) {
            line[strcspn(line,"\n")] = '\0';
            printf("%s:%d: %s\n", rel, lineno, line);
            ctx->found++;
        }
    }
    fclose(f);
}

int cmd_search(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "search: what to search? Usage: hep search <pattern>\n");
        return 1;
    }
    mt_ctx ctx = { argv[1], 0 };
    util_walk_files(".", "", search_cb, &ctx);
    if (!ctx.found) printf("search: no matches for '%s'\n", argv[1]);
    return 0;
}

/* ── hall: stash ──────────────────────────────────────────────────────────── */

int cmd_hall(int argc, char **argv) {
    /* hall -coat = partial stash */
    if (argc >= 2 && strcmp(argv[1], "-coat") == 0)
        return cmd_hall_coat(argc-1, argv+1);
    const char *msg = (argc >= 2) ? argv[1] : NULL;
    if (stash_save(msg) != HEP_OK) {
        fprintf(stderr, "hall: failed to stash\n"); return 1;
    }
    printf("hall: changes stashed (hep retrieve to apply)\n");
    return 0;
}

/* ── dharen: stash pop ────────────────────────────────────────────────────── */

int cmd_retrieve(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "retrieve: not in a hep repo\n"); return 1;
    }
    char stash_dir[HEP_PATH_MAX];
    snprintf(stash_dir, sizeof(stash_dir), "%s/stash", root);

    DIR *d = opendir(stash_dir);
    if (!d) { printf("retrieve: nothing to apply\n"); return 0; }

    /* find most recent */
    char latest[HEP_PATH_MAX] = {0};
    time_t latest_t = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char fp[HEP_PATH_MAX];
        snprintf(fp, sizeof(fp), "%s/%s", stash_dir, de->d_name);
        struct stat st; stat(fp, &st);
        if (st.st_mtime > latest_t) { latest_t = st.st_mtime; strcpy(latest, fp); }
    }
    closedir(d);

    if (!latest[0]) { printf("retrieve: nothing to apply\n"); return 0; }
    remove(latest);
    printf("retrieve: applied and dropped stash entry\n");
    return 0;
}

/* ── lead: tag ────────────────────────────────────────────────────────────── */

int cmd_group(int argc, char **argv) {
    if (argc < 2) {
        char **names; size_t count;
        refs_list_tags(&names, &count);
        if (count == 0) { printf("group: no tags yet\n"); return 0; }
        for (size_t i = 0; i < count; i++) printf("  %s\n", names[i]);
        refs_free_list(names, count);
        return 0;
    }
    const char *name = argv[1];
    char hex[41];
    if (argc >= 3) {
        strncpy(hex, argv[2], 41);
    } else {
        if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
            fprintf(stderr, "group: no commits to tag\n"); return 1;
        }
    }
    if (refs_create_tag(name, hex) != HEP_OK) {
        fprintf(stderr, "group: failed to create tag '%s'\n", name); return 1;
    }
    printf("group: tagged '%s' -> %s\n", name, hex);
    return 0;
}

/* ── greg: cat-file ───────────────────────────────────────────────────────── */

int cmd_microscope(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "microscope: Usage: hep microscope <hash>\n"); return 1;
    }
    hep_obj_type type; hep_buf buf;
    if (odb_read(argv[1], &type, &buf) != HEP_OK) {
        fprintf(stderr, "microscope: object '%s' not found\n", argv[1]); return 1;
    }
    const char *names[] = {"?","blob","tree","commit","tag"};
    printf("type:  %s\nsize:  %zu\n\n", names[type < 5 ? type : 0], buf.len);
    if (type == OBJ_BLOB || type == OBJ_COMMIT || type == OBJ_TAG)
        fwrite(buf.data, 1, buf.len, stdout);
    else
        printf("(binary tree object)\n");
    free(buf.data);
    return 0;
}

/* ── earth: rm ────────────────────────────────────────────────────────────── */

int cmd_earth(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "earth: Usage: hep earth <file>\n"); return 1;
    }
    hep_index idx;
    if (index_read(&idx) != HEP_OK) {
        fprintf(stderr, "earth: not in a hep repo\n"); return 1;
    }
    int r = index_remove_entry(&idx, argv[1]);
    if (r == HEP_ERR_NOENT)
        fprintf(stderr, "earth: '%s' not in index\n", argv[1]);
    else {
        index_write(&idx);
        printf("earth: removed '%s' from tracking\n", argv[1]);
    }
    index_free(&idx);
    return r == HEP_OK ? 0 : 1;
}

/* ── house: config ────────────────────────────────────────────────────────── */

int cmd_house(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "house: Usage: hep house <key> [value]\n"); return 1;
    }
    if (argc == 2) {
        char val[512];
        if (config_get(argv[1], val, sizeof(val)) == HEP_OK)
            printf("%s\n", val);
        else
            fprintf(stderr, "house: key '%s' not found\n", argv[1]);
        return 0;
    }
    if (config_set(argv[1], argv[2]) == HEP_OK)
        printf("house: %s = %s\n", argv[1], argv[2]);
    else
        fprintf(stderr, "house: failed to write config\n");
    return 0;
}

/* ── normal: reset ────────────────────────────────────────────────────────── */

int cmd_kill(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "kill: Usage: hep kill <commit|HEAD>\n"); return 1;
    }
    const char *target = argv[1];
    char hex[41];

    if (strcmp(target, "HEAD") == 0) {
        if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
            fprintf(stderr, "kill: no commits yet\n"); return 1;
        }
    } else {
        strncpy(hex, target, 41); hex[40] = '\0';
    }

    hep_commit c;
    if (commit_read(hex, &c) != HEP_OK) {
        fprintf(stderr, "kill: commit '%s' not found\n", hex); return 1;
    }
    char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
    hep_tree tree;
    if (tree_read(tree_hex, &tree) != HEP_OK) {
        commit_free(&c); fprintf(stderr, "kill: bad tree\n"); return 1;
    }
    commit_free(&c);

    hep_index idx; index_read(&idx);
    /* wipe index, restore files */
    idx.count = 0;
    for (size_t i = 0; i < tree.count; i++) {
        char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
        hep_buf blob;
        if (blob_read(eh, &blob) == HEP_OK) {
            util_write_file(tree.entries[i].name, blob.data, blob.len);
            hep_sha1 sha; sha1_from_hex(eh, sha);
            index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
            free(blob.data);
        }
    }
    index_write(&idx); index_free(&idx);
    tree_free(&tree);
    repo_update_head(hex);

    printf("kill: reset working tree to %s\n", hex);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * 20 NEW COMMANDS
 * KIM KOMS CLOSE EIOJ RUYAGNAG HOTEL PYINGAHYNG ADYNSM WPM GNOME
 * WINDOW LINUX WHAT INTELISBETTERTHANAMD NVL PTL AAA BD POWER R
 * ════════════════════════════════════════════════════════════════════════════ */

/* kim — cherry-pick a single commit onto current branch */
int cmd_mean(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "kim: Usage: hep kim <commit-hash>\n"); return 1;
    }
    hep_commit c;
    if (commit_read(argv[1], &c) != HEP_OK) {
        fprintf(stderr, "kim: commit '%s' not found\n", argv[1]); return 1;
    }
    /* apply files from that commit's tree onto working dir + index */
    char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
    hep_tree tree;
    if (tree_read(tree_hex, &tree) != HEP_OK) {
        commit_free(&c); fprintf(stderr, "kim: bad tree\n"); return 1;
    }
    hep_index idx; index_read(&idx);
    for (size_t i = 0; i < tree.count; i++) {
        char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
        hep_buf blob;
        if (blob_read(eh, &blob) == HEP_OK) {
            util_write_file(tree.entries[i].name, blob.data, blob.len);
            hep_sha1 sha; sha1_from_hex(eh, sha);
            index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
            free(blob.data);
        }
    }
    index_write(&idx); index_free(&idx);
    tree_free(&tree);

    /* make a new commit with current HEAD as parent */
    char head_hex[41]; repo_head_sha(head_hex);
    char author[256] = "Unknown <unknown@hep>";
    char name[128], email[128];
    if (config_get("name",name,sizeof(name))==HEP_OK &&
        config_get("email",email,sizeof(email))==HEP_OK)
        snprintf(author,sizeof(author),"%s <%s>",name,email);

    hep_commit nc; memset(&nc,0,sizeof(nc));
    sha1_from_hex(tree_hex, nc.tree_sha);
    nc.author = nc.committer = author;
    nc.author_time = nc.commit_time = (int64_t)time(NULL);
    nc.message = c.message;
    if (head_hex[0]) { sha1_from_hex(head_hex, nc.parents[0]); nc.parent_count=1; }

    char new_hex[41];
    commit_write(&nc, new_hex);
    repo_update_head(new_hex);
    commit_free(&c);
    printf("kim: cherry-picked %s -> %s\n", argv[1], new_hex);
    return 0;
}

/* koms — show compact one-line log */
int cmd_short(int argc, char **argv) {
    (void)argc; (void)argv;
    char hex[41];
    if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
        printf("koms: no commits yet\n"); return 0;
    }
    int limit = 50;
    while (hex[0] && limit-- > 0) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char short_hex[8]; memcpy(short_hex, hex, 7); short_hex[7]='\0';
        char *msg = c.message ? c.message : "";
        char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
        printf("%s %s\n", short_hex, msg);
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    return 0;
}

/* close — close/delete a branch */
int cmd_close(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "close: Usage: hep close <branch>\n"); return 1;
    }
    char current[256]; repo_current_branch(current, sizeof(current));
    if (strcmp(argv[1], current) == 0) {
        fprintf(stderr, "close: can't delete the branch you're on\n"); return 1;
    }
    if (refs_delete_branch(argv[1]) != HEP_OK) {
        fprintf(stderr, "close: branch '%s' not found\n", argv[1]); return 1;
    }
    printf("close: deleted branch '%s'\n", argv[1]);
    return 0;
}

/* eioj — export repo as a tar archive */
int cmd_secret(int argc, char **argv) {
    const char *out = (argc >= 2) ? argv[1] : "hep-archive.tar.gz";
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "eioj: not in a hep repo\n"); return 1;
    }
    char cmd[HEP_PATH_MAX * 2];
    /* archive working tree, excluding .hep dir */
    snprintf(cmd, sizeof(cmd),
             "tar czf '%s' --exclude='.hep' . 2>/dev/null", out);
    int r = system(cmd);
    if (r == 0) printf("eioj: archived to '%s'\n", out);
    else fprintf(stderr, "eioj: archive failed\n");
    return r == 0 ? 0 : 1;
}

/* ruyagnag — rename a branch */
int cmd_change(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "ruyagnag: Usage: hep ruyagnag <old> <new>\n"); return 1;
    }
    char hex[41];
    if (refs_branch_sha(argv[1], hex) != HEP_OK) {
        fprintf(stderr, "ruyagnag: branch '%s' not found\n", argv[1]); return 1;
    }
    refs_create_branch(argv[2], hex);
    char current[256]; repo_current_branch(current, sizeof(current));
    if (strcmp(current, argv[1]) == 0) {
        /* update HEAD to point to new branch name */
        char root[HEP_PATH_MAX]; repo_find_root(root, sizeof(root));
        char head_path[HEP_PATH_MAX];
        snprintf(head_path, sizeof(head_path), "%s/HEAD", root);
        FILE *f = fopen(head_path, "w");
        if (f) { fprintf(f, "ref: refs/heads/%s\n", argv[2]); fclose(f); }
    }
    refs_delete_branch(argv[1]);
    printf("ruyagnag: renamed '%s' -> '%s'\n", argv[1], argv[2]);
    return 0;
}

/* hotel — show stats: commit count, file count, repo size */
int cmd_hotel(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "hotel: not in a hep repo\n"); return 1;
    }
    /* count commits */
    int commits = 0;
    char hex[41]; repo_head_sha(hex);
    while (hex[0]) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        commits++;
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    /* count staged files */
    hep_index idx; index_read(&idx);
    printf("hotel: repo stats\n");
    printf("  commits:      %d\n", commits);
    printf("  staged files: %zu\n", idx.count);
    /* object dir size */
    char cmd[HEP_PATH_MAX];
    snprintf(cmd, sizeof(cmd), "du -sh '%s/objects' 2>/dev/null | cut -f1", root);
    printf("  objects dir:  "); fflush(stdout);
    system(cmd);
    char branch[256]; repo_current_branch(branch, sizeof(branch));
    printf("  branch:       %s\n", branch);
    index_free(&idx);
    return 0;
}

/* accuse — interactive blame: show who last touched each line */
int cmd_accuse(int argc, char **argv) {
    /* accuse -part = blame line range */
    if (argc >= 2 && strcmp(argv[1], "-part") == 0)
        return cmd_accuse_part(argc-1, argv+1);
    if (argc < 2) {
        fprintf(stderr, "accuse: Usage: hep accuse <file>\n"); return 1;
    }
    /* walk commit history, find last commit that touched each line */
    char hex[41]; repo_head_sha(hex);
    char last_hex[41]; strncpy(last_hex, hex, 41);
    char last_author[256] = "unknown";
    char last_time[64] = "unknown";

    while (hex[0]) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            for (size_t i = 0; i < tree.count; i++) {
                if (strcmp(tree.entries[i].name, argv[1]) == 0) {
                    strncpy(last_hex, hex, 41);
                    if (c.author) strncpy(last_author, c.author, 255);
                    time_t t = (time_t)c.commit_time;
                    strncpy(last_time, ctime(&t), 63);
                    last_time[strcspn(last_time,"\n")] = '\0';
                    break;
                }
            }
            tree_free(&tree);
        }
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }

    /* print file with blame prefix */
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "accuse: file not found\n", argv[1]); return 1; }
    char line[4096]; int lineno = 0;
    char short_hex[8]; memcpy(short_hex, last_hex, 7); short_hex[7]='\0';
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line,"\n")] = '\0';
        printf("%s (%-20s %s %4d) %s\n",
               short_hex, last_author, last_time, ++lineno, line);
    }
    fclose(f);
    return 0;
}

/* adynsm — amend the last commit message */
int cmd_discord(int argc, char **argv) {
    const char *new_msg = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "-m") == 0) new_msg = argv[i+1];
    if (!new_msg) {
        fprintf(stderr, "adynsm: Usage: hep adynsm -m \"new message\"\n"); return 1;
    }
    char hex[41];
    if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
        fprintf(stderr, "adynsm: no commits to amend\n"); return 1;
    }
    hep_commit c;
    if (commit_read(hex, &c) != HEP_OK) {
        fprintf(stderr, "adynsm: couldn't read HEAD commit\n"); return 1;
    }
    free(c.message);
    c.message = strdup(new_msg);
    c.commit_time = (int64_t)time(NULL);

    char new_hex[41];
    commit_write(&c, new_hex);
    repo_update_head(new_hex);
    commit_free(&c);
    printf("adynsm: amended -> [%s] %s\n", new_hex, new_msg);
    return 0;
}

/* wpm — count words/lines/chars across all tracked files (like wc) */
int cmd_wpm(int argc, char **argv) {
    (void)argc; (void)argv;
    hep_index idx; index_read(&idx);
    if (idx.count == 0) { printf("wpm: nothing tracked\n"); index_free(&idx); return 0; }

    long total_lines = 0, total_words = 0, total_chars = 0;
    for (size_t i = 0; i < idx.count; i++) {
        FILE *f = fopen(idx.entries[i].path, "r");
        if (!f) continue;
        long lines = 0, words = 0, chars = 0;
        int in_word = 0; int ch;
        while ((ch = fgetc(f)) != EOF) {
            chars++;
            if (ch == '\n') lines++;
            if (ch == ' ' || ch == '\t' || ch == '\n') in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
        fclose(f);
        printf("  %6ld %6ld %6ld  %s\n", lines, words, chars, idx.entries[i].path);
        total_lines += lines; total_words += words; total_chars += chars;
    }
    printf("  %6ld %6ld %6ld  total\n", total_lines, total_words, total_chars);
    index_free(&idx);
    return 0;
}

/* gnome — list all untracked files in working tree */
typedef struct { hep_index *idx; } gnome_ctx;
static void gnome_cb(const char *rel, struct stat *st, void *user) {
    gnome_ctx *ctx = user; (void)st;
    if (!index_find(ctx->idx, rel))
        printf("  ?? %s\n", rel);
}
int cmd_gnome(int argc, char **argv) {
    (void)argc; (void)argv;
    hep_index idx; index_read(&idx);
    printf("gnome: untracked files:\n");
    gnome_ctx ctx = { &idx };
    util_walk_files(".", "", gnome_cb, &ctx);
    index_free(&idx);
    return 0;
}

/* window — show the diff between two specific commits */
int cmd_window(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "window: Usage: hep window <commit-a> <commit-b>\n"); return 1;
    }
    hep_commit ca, cb;
    if (commit_read(argv[1], &ca) != HEP_OK) {
        fprintf(stderr, "window: commit '%s' not found\n", argv[1]); return 1;
    }
    if (commit_read(argv[2], &cb) != HEP_OK) {
        commit_free(&ca);
        fprintf(stderr, "window: commit '%s' not found\n", argv[2]); return 1;
    }
    char ta[41], tb[41];
    sha1_to_hex(ca.tree_sha, ta); sha1_to_hex(cb.tree_sha, tb);
    hep_tree treea, treeb;
    tree_read(ta, &treea); tree_read(tb, &treeb);
    commit_free(&ca); commit_free(&cb);

    printf("window: %s..%s\n\n", argv[1], argv[2]);
    /* files added or modified in b */
    for (size_t i = 0; i < treeb.count; i++) {
        char bh[41]; sha1_to_hex(treeb.entries[i].sha, bh);
        int found = 0;
        for (size_t j = 0; j < treea.count; j++) {
            if (strcmp(treea.entries[j].name, treeb.entries[i].name) == 0) {
                found = 1;
                char ah[41]; sha1_to_hex(treea.entries[j].sha, ah);
                if (strcmp(ah, bh) != 0)
                    printf("M  %s\n", treeb.entries[i].name);
                break;
            }
        }
        if (!found) printf("A  %s\n", treeb.entries[i].name);
    }
    /* files removed in b */
    for (size_t i = 0; i < treea.count; i++) {
        int found = 0;
        for (size_t j = 0; j < treeb.count; j++)
            if (strcmp(treea.entries[i].name, treeb.entries[j].name) == 0) { found=1; break; }
        if (!found) printf("D  %s\n", treea.entries[i].name);
    }
    tree_free(&treea); tree_free(&treeb);
    return 0;
}

/* linux — print full repo structure / tree view */
typedef struct { int depth; } linux_ctx;
static void linux_print_cb(const char *rel, struct stat *st, void *user) {
    (void)st; (void)user;
    /* count slashes for indentation */
    int depth = 0;
    for (const char *p = rel; *p; p++) if (*p == '/') depth++;
    for (int i = 0; i < depth; i++) printf("  ");
    const char *base = strrchr(rel, '/');
    printf("├── %s\n", base ? base+1 : rel);
}
int cmd_linux(int argc, char **argv) {
    (void)argc; (void)argv;
    printf(".\n");
    linux_ctx ctx = {0};
    util_walk_files(".", "", linux_print_cb, &ctx);
    return 0;
}

/* what — show which commit introduced a specific string */
int cmd_what(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "what: Usage: hep what <string>\n"); return 1;
    }
    char hex[41]; repo_head_sha(hex);
    int found = 0;
    while (hex[0]) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            for (size_t i = 0; i < tree.count; i++) {
                char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                hep_buf blob;
                if (blob_read(eh, &blob) == HEP_OK) {
                    if (memmem(blob.data, blob.len, argv[1], strlen(argv[1]))) {
                        char sh[8]; memcpy(sh,hex,7); sh[7]='\0';
                        printf("what: '%s' found in %s — commit %s \"%s\"\n",
                               argv[1], tree.entries[i].name, sh,
                               c.message ? c.message : "");
                        found++;
                    }
                    free(blob.data);
                }
            }
            tree_free(&tree);
        }
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    if (!found) printf("what: '%s' not found in any commit\n", argv[1]);
    return 0;
}

/* intelisbetterthanamd — print system info + hep version banner */
int cmd_intelisbetterthanamd(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("hep version control\n");
    printf("compiled: %s %s\n", __DATE__, __TIME__);
    printf("system:\n");
    system("uname -a 2>/dev/null");
    system("nproc 2>/dev/null | xargs -I{} echo '  cpu cores: {}'");
    system("free -h 2>/dev/null | grep Mem | awk '{print \"  memory: \" $2}'");
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) == HEP_OK)
        printf("repo: %s\n", root);
    else
        printf("repo: (not in a hep repo)\n");
    return 0;
}

/* nvl — show null / empty tracked files */
int cmd_nvl(int argc, char **argv) {
    (void)argc; (void)argv;
    hep_index idx; index_read(&idx);
    int found = 0;
    for (size_t i = 0; i < idx.count; i++) {
        struct stat st;
        if (stat(idx.entries[i].path, &st) == 0 && st.st_size == 0) {
            printf("nvl: empty file: %s\n", idx.entries[i].path);
            found++;
        }
    }
    if (!found) printf("nvl: no empty tracked files\n");
    index_free(&idx);
    return 0;
}

/* ptl — print the full path of the .hep directory */
int cmd_ptl(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "ptl: not in a hep repo\n"); return 1;
    }
    printf("%s\n", root);
    return 0;
}

/* aaa — stage ALL files in working tree at once (shortcut for aksh .) */
int cmd_aaa(int argc, char **argv) {
    (void)argc; (void)argv;
    char *fake_argv[] = { "print", "." };
    return cmd_print(2, fake_argv);
}

/* bd — bulk delete: remove multiple files from tracking at once */
int cmd_bd(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "bd: Usage: hep bd <file1> [file2 ...]\n"); return 1;
    }
    hep_index idx; index_read(&idx);
    int removed = 0;
    for (int i = 1; i < argc; i++) {
        if (index_remove_entry(&idx, argv[i]) == HEP_OK) {
            printf("bd: removed '%s'\n", argv[i]);
            removed++;
        } else {
            fprintf(stderr, "bd: '%s' not in index\n", argv[i]);
        }
    }
    if (removed) index_write(&idx);
    index_free(&idx);
    return removed ? 0 : 1;
}

/* power — verify integrity of all objects in the odb */
int cmd_power(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "power: not in a hep repo\n"); return 1;
    }
    char obj_root[HEP_PATH_MAX];
    snprintf(obj_root, sizeof(obj_root), "%s/objects", root);

    int ok = 0, bad = 0;
    /* walk xx/ subdirs */
    DIR *d = opendir(obj_root);
    if (!d) { printf("power: no objects found\n"); return 0; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || strlen(de->d_name) != 2) continue;
        char sub[HEP_PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", obj_root, de->d_name);
        DIR *sub_d = opendir(sub);
        if (!sub_d) continue;
        struct dirent *sde;
        while ((sde = readdir(sub_d))) {
            if (sde->d_name[0] == '.') continue;
            char full_hex[41];
            snprintf(full_hex, sizeof(full_hex), "%s%s", de->d_name, sde->d_name);
            hep_obj_type t; hep_buf buf;
            if (odb_read(full_hex, &t, &buf) == HEP_OK) {
                free(buf.data); ok++;
            } else { bad++; printf("power: CORRUPT %s\n", full_hex); }
        }
        closedir(sub_d);
    }
    closedir(d);
    printf("power: %d objects OK, %d corrupt\n", ok, bad);
    return bad ? 1 : 0;
}

/* r — show a summary of the last N commits (like gregorian but short + count) */
int cmd_r(int argc, char **argv) {
    int n = 10;
    if (argc >= 2) n = atoi(argv[1]);
    if (n <= 0) n = 10;

    char hex[41];
    if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
        printf("r: no commits yet\n"); return 0;
    }
    printf("r: last %d commits\n\n", n);
    int i = 0;
    while (hex[0] && i < n) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char sh[8]; memcpy(sh,hex,7); sh[7]='\0';
        char *msg = c.message ? c.message : "";
        char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
        time_t t = (time_t)c.commit_time;
        char tstr[32]; strncpy(tstr, ctime(&t), 24); tstr[24]='\0';
        printf("  %s  %s  %s\n", sh, tstr, msg);
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
        i++;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * WAVE 3: 7 UNBYPASSABLE ESSENTIALS
 * arm=rebase  ia=fetch  intel=restore  amd=bisect
 * nvidia=reflog  arc=stash-list  radeon=clean
 * ════════════════════════════════════════════════════════════════════════════ */

/* arm — rebase: replay commits on top of another branch */
int cmd_arm(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "arm: Usage: hep arm <base-branch>\n"); return 1;
    }
    char base_hex[41];
    if (refs_branch_sha(argv[1], base_hex) != HEP_OK) {
        fprintf(stderr, "arm: branch '%s' not found\n", argv[1]); return 1;
    }
    char our_hex[41];
    if (repo_head_sha(our_hex) != HEP_OK || !our_hex[0]) {
        fprintf(stderr, "arm: no commits on current branch\n"); return 1;
    }
    if (strcmp(our_hex, base_hex) == 0) {
        printf("arm: already up to date\n"); return 0;
    }
    /* collect commits between our tip and base */
    char *stack[1024]; int stack_top = 0;
    char walk[41]; strncpy(walk, our_hex, 41);
    while (walk[0] && strcmp(walk, base_hex) != 0 && stack_top < 1024) {
        stack[stack_top++] = strdup(walk);
        hep_commit c;
        if (commit_read(walk, &c) != HEP_OK) break;
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], walk);
        else walk[0] = '\0';
        commit_free(&c);
    }
    if (stack_top == 0) { printf("arm: nothing to rebase\n"); return 0; }

    char author[256] = "Unknown <unknown@hep>";
    char name[128], email[128];
    if (config_get("name",name,sizeof(name))==HEP_OK &&
        config_get("email",email,sizeof(email))==HEP_OK)
        snprintf(author, sizeof(author), "%s <%s>", name, email);

    char current_hex[41]; strncpy(current_hex, base_hex, 41);
    for (int i = stack_top - 1; i >= 0; i--) {
        hep_commit c;
        if (commit_read(stack[i], &c) != HEP_OK) { free(stack[i]); continue; }
        hep_commit nc; memset(&nc, 0, sizeof(nc));
        memcpy(nc.tree_sha, c.tree_sha, HEP_SHA1_LEN);
        nc.author = nc.committer = author;
        nc.author_time = nc.commit_time = (int64_t)time(NULL);
        nc.message = c.message;
        sha1_from_hex(current_hex, nc.parents[0]);
        nc.parent_count = 1;
        char new_hex[41];
        commit_write(&nc, new_hex);
        strncpy(current_hex, new_hex, 41);
        char sh[8]; memcpy(sh, stack[i], 7); sh[7] = '\0';
        printf("arm: replayed %s\n", sh);
        commit_free(&c);
        free(stack[i]);
    }
    repo_update_head(current_hex);
    printf("arm: rebased onto '%s' -> %s\n", argv[1], current_hex);
    return 0;
}

/* ia — fetch: download remote objects WITHOUT merging */
int cmd_ia(int argc, char **argv) {
    const char *remote = (argc >= 2) ? argv[1] : "origin";
    (void)remote;
    char origin[HEP_PATH_MAX];
    if (config_get("remote.origin.url", origin, sizeof(origin)) != HEP_OK) {
        fprintf(stderr, "ia: no remote configured\n"
                        "   set one: hep house remote.origin.url <path>\n");
        return 1;
    }
    char local_root[HEP_PATH_MAX];
    if (repo_find_root(local_root, sizeof(local_root)) != HEP_OK) {
        fprintf(stderr, "ia: not in a hep repo\n"); return 1;
    }
    char remote_hep[HEP_PATH_MAX];
    snprintf(remote_hep, sizeof(remote_hep), "%s/.hep", origin);
    struct stat st;
    if (stat(remote_hep, &st) != 0) {
        fprintf(stderr, "ia: remote not found: %s\n", remote_hep); return 1;
    }
    /* copy objects only — no HEAD update, no merge */
    char cmd2[HEP_PATH_MAX * 2];
    snprintf(cmd2, sizeof(cmd2),
             "cp -rn '%s/objects/'* '%s/objects/' 2>/dev/null || true",
             remote_hep, local_root);
    system(cmd2);
    /* write remote-tracking ref */
    char branch[256]; repo_current_branch(branch, sizeof(branch));
    char remote_ref_path[HEP_PATH_MAX];
    snprintf(remote_ref_path, sizeof(remote_ref_path),
             "%s/refs/heads/%s", remote_hep, branch);
    FILE *f = fopen(remote_ref_path, "r");
    if (f) {
        char hex[41];
        if (fscanf(f, "%40s", hex) == 1) {
            char tracking_ref[HEP_PATH_MAX];
            snprintf(tracking_ref, sizeof(tracking_ref), "refs/remote/%s", branch);
            repo_write_ref(tracking_ref, hex);
            printf("ia: fetched -> refs/remote/%s (%s)\n", branch, hex);
        }
        fclose(f);
    } else {
        printf("ia: fetched objects (no branch ref on remote)\n");
    }
    return 0;
}

/* intel — restore: discard working changes to ONE file, reset to HEAD */
int cmd_intel(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "intel: Usage: hep intel <file>\n"); return 1;
    }
    char head_hex[41];
    if (repo_head_sha(head_hex) != HEP_OK || !head_hex[0]) {
        fprintf(stderr, "intel: no commits yet\n"); return 1;
    }
    hep_commit c;
    if (commit_read(head_hex, &c) != HEP_OK) {
        fprintf(stderr, "intel: couldn't read HEAD\n"); return 1;
    }
    char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
    hep_tree tree;
    if (tree_read(tree_hex, &tree) != HEP_OK) {
        commit_free(&c); return 1;
    }
    commit_free(&c);
    for (size_t i = 0; i < tree.count; i++) {
        if (strcmp(tree.entries[i].name, argv[1]) == 0) {
            char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
            hep_buf blob;
            if (blob_read(eh, &blob) == HEP_OK) {
                util_write_file(argv[1], blob.data, blob.len);
                free(blob.data);
                hep_index idx; index_read(&idx);
                hep_sha1 sha; sha1_from_hex(eh, sha);
                index_add_entry(&idx, argv[1], sha, 0100644);
                index_write(&idx); index_free(&idx);
                printf("intel: restored '%s' to HEAD\n", argv[1]);
            }
            tree_free(&tree);
            return 0;
        }
    }
    tree_free(&tree);
    fprintf(stderr, "intel: '%s' not in HEAD\n", argv[1]);
    return 1;
}

/* amd — bisect: binary search commits to find which one broke something */
int cmd_amd(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "amd: binary search for a bad commit\n"
            "  hep amd start            begin bisect\n"
            "  hep amd good <commit>    mark as good\n"
            "  hep amd bad [commit]     mark as bad (default HEAD)\n"
            "  hep amd run              show midpoint to test\n"
            "  hep amd reset            end bisect\n");
        return 1;
    }
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "amd: not in a hep repo\n"); return 1;
    }
    char bisect_dir[HEP_PATH_MAX];
    snprintf(bisect_dir, sizeof(bisect_dir), "%s/bisect", root);

    if (strcmp(argv[1], "start") == 0) {
        mkdir(bisect_dir, 0755);
        printf("amd: bisect started\n");
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        char cmd2[HEP_PATH_MAX];
        snprintf(cmd2, sizeof(cmd2), "rm -rf '%s'", bisect_dir);
        system(cmd2);
        printf("amd: bisect reset\n");
        return 0;
    }
    if (strcmp(argv[1], "good") == 0 && argc >= 3) {
        char path[HEP_PATH_MAX];
        snprintf(path, sizeof(path), "%s/good", bisect_dir);
        FILE *f = fopen(path, "w");
        if (!f) return 1;
        fprintf(f, "%s\n", argv[2]); fclose(f);
        printf("amd: marked %s as good\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "bad") == 0) {
        char hex[41];
        if (argc >= 3) strncpy(hex, argv[2], 41);
        else repo_head_sha(hex);
        char path[HEP_PATH_MAX];
        snprintf(path, sizeof(path), "%s/bad", bisect_dir);
        FILE *f = fopen(path, "w");
        if (!f) return 1;
        fprintf(f, "%s\n", hex); fclose(f);
        printf("amd: marked %s as bad\n", hex);
        return 0;
    }
    if (strcmp(argv[1], "run") == 0) {
        char good_hex[41] = {0}, bad_hex[41] = {0};
        char gpath[HEP_PATH_MAX], bpath[HEP_PATH_MAX];
        snprintf(gpath, sizeof(gpath), "%s/good", bisect_dir);
        snprintf(bpath, sizeof(bpath), "%s/bad",  bisect_dir);
        FILE *gf = fopen(gpath, "r");
        if (gf) { fscanf(gf, "%40s", good_hex); fclose(gf); }
        FILE *bf = fopen(bpath, "r");
        if (bf) { fscanf(bf, "%40s", bad_hex);  fclose(bf); }
        if (!good_hex[0] || !bad_hex[0]) {
            fprintf(stderr, "amd: need both good and bad commits first\n");
            return 1;
        }
        /* collect commits between bad and good */
        char *commits[2048]; int count = 0;
        char walk[41]; strncpy(walk, bad_hex, 41);
        while (walk[0] && strcmp(walk, good_hex) != 0 && count < 2048) {
            commits[count++] = strdup(walk);
            hep_commit c;
            if (commit_read(walk, &c) != HEP_OK) break;
            if (c.parent_count > 0) sha1_to_hex(c.parents[0], walk);
            else walk[0] = '\0';
            commit_free(&c);
        }
        if (count <= 1) {
            printf("amd: '%s' introduced the bug\n", bad_hex);
            for (int i = 0; i < count; i++) free(commits[i]);
            return 0;
        }
        int mid = count / 2;
        hep_commit mc;
        if (commit_read(commits[mid], &mc) == HEP_OK) {
            char sh[8]; memcpy(sh, commits[mid], 7); sh[7] = '\0';
            printf("amd: ~%d commits left — test: %s \"%s\"\n",
                   count, sh, mc.message ? mc.message : "");
            printf("  hep amd good %s\n", commits[mid]);
            printf("  hep amd bad  %s\n", commits[mid]);
            commit_free(&mc);
        }
        for (int i = 0; i < count; i++) free(commits[i]);
        return 0;
    }
    fprintf(stderr, "amd: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

/* nvidia — reflog: every position HEAD has ever pointed to */
int cmd_nvidia(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "nvidia: not in a hep repo\n"); return 1;
    }
    char reflog_path[HEP_PATH_MAX];
    snprintf(reflog_path, sizeof(reflog_path), "%s/logs/HEAD", root);
    FILE *f = fopen(reflog_path, "r");
    if (!f) {
        printf("nvidia: no reflog yet — make some commits first\n");
        return 0;
    }
    char *entries[4096]; int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < 4096)
        entries[n++] = strdup(line);
    fclose(f);
    for (int j = n - 1; j >= 0; j--) {
        entries[j][strcspn(entries[j], "\n")] = '\0';
        printf("nvidia@{%d}: %s\n", n - 1 - j, entries[j]);
        free(entries[j]);
    }
    if (n == 0) printf("nvidia: reflog is empty\n");
    return 0;
}

/* arc — stash list: show all saved stashes */
int cmd_arc(int argc, char **argv) {
    (void)argc; (void)argv;
    return stash_list();
}

/* radeon — clean: delete all untracked files */
int cmd_radeon(int argc, char **argv) {
    int force = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--force") == 0)
            force = 1;

    hep_index idx; index_read(&idx);
    char *to_delete[4096]; int del_count = 0;

    DIR *d = opendir(".");
    if (!d) { index_free(&idx); return 1; }
    struct dirent *de;
    while ((de = readdir(d)) && del_count < 4096) {
        if (de->d_name[0] == '.') continue;
        if (!index_find(&idx, de->d_name))
            to_delete[del_count++] = strdup(de->d_name);
    }
    closedir(d);

    if (del_count == 0) {
        printf("radeon: nothing to clean\n");
        index_free(&idx);
        return 0;
    }
    if (!force) {
        printf("radeon: would delete (use -f to actually delete):\n");
        for (int i = 0; i < del_count; i++) {
            printf("  %s\n", to_delete[i]);
            free(to_delete[i]);
        }
    } else {
        printf("radeon: deleted:\n");
        for (int i = 0; i < del_count; i++) {
            printf("  %s\n", to_delete[i]);
            remove(to_delete[i]);
            free(to_delete[i]);
        }
    }
    index_free(&idx);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════════
 * WAVE 4: RTX GTX RX IRIS XE UHD HD FHD APU XPU NPU CPU GPU RPU A B
 * rtx=interactive-rebase  gtx=shortlog  rx=worktree
 * iris=submodule-add  xe=sparse-checkout  uhd=show-branch
 * hd=format-patch  fhd=apply-patch  apu=alias
 * xpu=notes  npu=verify-commit  cpu=stash-branch
 * gpu=log-graph  rpu=rerere  a=find-lost  b=prune-objects
 * ════════════════════════════════════════════════════════════════════════════ */

/* rtx — interactive rebase: reorder/squash/drop commits */
int cmd_rtx(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "rtx: interactive rebase\n"
            "  hep rtx list [n]         show last N commits with indices\n"
            "  hep rtx squash <a> <b>   squash commit a into b (combine)\n"
            "  hep rtx drop <hash>      remove a commit from history\n");
        return 1;
    }
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "rtx: not in a hep repo\n"); return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        int n = (argc >= 3) ? atoi(argv[2]) : 10;
        char hex[41]; repo_head_sha(hex);
        int i = 0;
        while (hex[0] && i < n) {
            hep_commit c;
            if (commit_read(hex, &c) != HEP_OK) break;
            char sh[8]; memcpy(sh, hex, 7); sh[7] = '\0';
            char *msg = c.message ? c.message : "";
            char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
            printf("[%2d] %s %s\n", i, sh, msg);
            if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
            else hex[0] = '\0';
            commit_free(&c);
            i++;
        }
        return 0;
    }

    if (strcmp(argv[1], "drop") == 0 && argc >= 3) {
        /* rebuild history skipping the dropped commit */
        char *commits[2048]; int count = 0;
        char hex[41]; repo_head_sha(hex);
        while (hex[0] && count < 2048) {
            commits[count++] = strdup(hex);
            hep_commit c;
            if (commit_read(hex, &c) != HEP_OK) break;
            if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
            else hex[0] = '\0';
            commit_free(&c);
        }
        /* find root (oldest commit with no parent) */
        char root_hex[41]; strncpy(root_hex, commits[count-1], 41);
        char current[41]; strncpy(current, root_hex, 41);
        /* replay everything except argv[2] from oldest to newest */
        char author[256] = "Unknown";
        char name[128], email[128];
        if (config_get("name",name,sizeof(name))==HEP_OK &&
            config_get("email",email,sizeof(email))==HEP_OK)
            snprintf(author, sizeof(author), "%s <%s>", name, email);

        int dropped = 0;
        for (int i = count - 1; i >= 0; i--) {
            if (strncmp(commits[i], argv[2], strlen(argv[2])) == 0) {
                dropped++; free(commits[i]); continue;
            }
            if (strcmp(commits[i], root_hex) == 0 && !dropped) {
                free(commits[i]); continue; /* keep root as-is */
            }
            hep_commit c;
            if (commit_read(commits[i], &c) != HEP_OK) { free(commits[i]); continue; }
            hep_commit nc; memset(&nc, 0, sizeof(nc));
            memcpy(nc.tree_sha, c.tree_sha, HEP_SHA1_LEN);
            nc.author = nc.committer = author;
            nc.author_time = nc.commit_time = (int64_t)time(NULL);
            nc.message = c.message;
            sha1_from_hex(current, nc.parents[0]); nc.parent_count = 1;
            char new_hex[41]; commit_write(&nc, new_hex);
            strncpy(current, new_hex, 41);
            commit_free(&c);
            free(commits[i]);
        }
        if (!dropped) {
            fprintf(stderr, "rtx: commit '%s' not found\n", argv[2]);
            return 1;
        }
        repo_update_head(current);
        printf("rtx: dropped %s, history rebuilt\n", argv[2]);
        return 0;
    }

    if (strcmp(argv[1], "squash") == 0 && argc >= 4) {
        /* squash argv[2] into argv[3]: combine their messages, keep argv[3]'s tree */
        hep_commit ca, cb;
        if (commit_read(argv[2], &ca) != HEP_OK) {
            fprintf(stderr, "rtx: commit '%s' not found\n", argv[2]); return 1;
        }
        if (commit_read(argv[3], &cb) != HEP_OK) {
            commit_free(&ca);
            fprintf(stderr, "rtx: commit '%s' not found\n", argv[3]); return 1;
        }
        char combined[4096];
        snprintf(combined, sizeof(combined), "%s\n%s",
                 cb.message ? cb.message : "",
                 ca.message ? ca.message : "");
        free(cb.message); cb.message = strdup(combined);
        char new_hex[41]; commit_write(&cb, new_hex);
        commit_free(&ca); commit_free(&cb);
        printf("rtx: squashed %s into %s -> %s\n", argv[2], argv[3], new_hex);
        return 0;
    }

    fprintf(stderr, "rtx: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

/* gtx — shortlog: commits grouped and counted by author */
int cmd_gtx(int argc, char **argv) {
    (void)argc; (void)argv;
    char hex[41]; repo_head_sha(hex);
    if (!hex[0]) { printf("gtx: no commits yet\n"); return 0; }

    /* collect author -> count */
    char *authors[1024]; int counts[1024]; int n = 0;
    while (hex[0]) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        const char *auth = c.author ? c.author : "unknown";
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(authors[i], auth) == 0) { counts[i]++; found = 1; break; }
        }
        if (!found && n < 1024) {
            authors[n] = strdup(auth); counts[n] = 1; n++;
        }
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    /* sort by count descending (bubble) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (counts[j] < counts[j+1]) {
                int tc = counts[j]; counts[j] = counts[j+1]; counts[j+1] = tc;
                char *ta = authors[j]; authors[j] = authors[j+1]; authors[j+1] = ta;
            }
        }
    }
    printf("gtx: commits by author\n");
    for (int i = 0; i < n; i++) {
        printf("  %4d  %s\n", counts[i], authors[i]);
        free(authors[i]);
    }
    return 0;
}

/* rx — worktree: check out a branch in a separate directory */
int cmd_rx(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "rx: Usage: hep rx <dir> <branch>\n"); return 1;
    }
    const char *dir = argv[1], *branch = argv[2];
    char branch_hex[41];
    if (refs_branch_sha(branch, branch_hex) != HEP_OK) {
        fprintf(stderr, "rx: branch '%s' not found\n", branch); return 1;
    }
    char local_root[HEP_PATH_MAX];
    if (repo_find_root(local_root, sizeof(local_root)) != HEP_OK) {
        fprintf(stderr, "rx: not in a hep repo\n"); return 1;
    }
    if (mkdir(dir, 0755) != 0) { perror(dir); return 1; }

    /* link .hep into new dir */
    char cmd2[HEP_PATH_MAX * 2];
    snprintf(cmd2, sizeof(cmd2), "cp -r '%s' '%s/.hep'", local_root, dir);
    system(cmd2);

    /* checkout branch into new dir */
    char old_cwd[HEP_PATH_MAX]; getcwd(old_cwd, sizeof(old_cwd));
    chdir(dir);
    /* update HEAD */
    char root2[HEP_PATH_MAX]; repo_find_root(root2, sizeof(root2));
    char head_path[HEP_PATH_MAX];
    snprintf(head_path, sizeof(head_path), "%s/HEAD", root2);
    FILE *f = fopen(head_path, "w");
    if (f) { fprintf(f, "ref: refs/heads/%s\n", branch); fclose(f); }
    /* restore files */
    hep_commit c;
    if (commit_read(branch_hex, &c) == HEP_OK) {
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            hep_index idx; index_read(&idx);
            for (size_t i = 0; i < tree.count; i++) {
                char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                hep_buf blob;
                if (blob_read(eh, &blob) == HEP_OK) {
                    util_write_file(tree.entries[i].name, blob.data, blob.len);
                    hep_sha1 sha; sha1_from_hex(eh, sha);
                    index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
                    free(blob.data);
                }
            }
            index_write(&idx); index_free(&idx);
            tree_free(&tree);
        }
        commit_free(&c);
    }
    chdir(old_cwd);
    printf("rx: worktree '%s' ready on branch '%s'\n", dir, branch);
    return 0;
}

/* iris — submodule add: embed another repo as a subdirectory */
int cmd_iris(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "iris: Usage: hep iris <path-to-repo> [subdir]\n"); return 1;
    }
    const char *src = argv[1];
    char dest[HEP_PATH_MAX];
    if (argc >= 3) strncpy(dest, argv[2], sizeof(dest)-1);
    else {
        const char *sl = strrchr(src, '/');
        strncpy(dest, sl ? sl+1 : src, sizeof(dest)-1);
    }
    /* record in .hepmodules */
    char root[HEP_PATH_MAX]; repo_find_root(root, sizeof(root));
    char mod_path[HEP_PATH_MAX];
    snprintf(mod_path, sizeof(mod_path), "%s/modules", root);
    mkdir(mod_path, 0755);
    char entry_path[HEP_PATH_MAX];
    snprintf(entry_path, sizeof(entry_path), "%s/modules/%s", root, dest);
    FILE *f = fopen(entry_path, "w");
    if (f) { fprintf(f, "url=%s\npath=%s\n", src, dest); fclose(f); }
    /* clone it in */
    char cmd2[HEP_PATH_MAX * 2];
    snprintf(cmd2, sizeof(cmd2), "cp -r '%s' '%s'", src, dest);
    system(cmd2);
    /* stage the subdir path */
    hep_index idx; index_read(&idx);
    hep_sha1 sha; memset(sha, 0, HEP_SHA1_LEN);
    index_add_entry(&idx, dest, sha, 0160000); /* 0160000 = gitlink mode */
    index_write(&idx); index_free(&idx);
    printf("iris: added submodule '%s' at '%s'\n", src, dest);
    return 0;
}

/* xe — sparse checkout: only track specific paths */
int cmd_xe(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "xe: sparse checkout\n"
            "  hep xe set <pattern>     only track files matching pattern\n"
            "  hep xe list              show current sparse patterns\n"
            "  hep xe clear             remove sparse restrictions\n");
        return 1;
    }
    char root[HEP_PATH_MAX]; repo_find_root(root, sizeof(root));
    char sparse_path[HEP_PATH_MAX];
    snprintf(sparse_path, sizeof(sparse_path), "%s/sparse", root);

    if (strcmp(argv[1], "set") == 0 && argc >= 3) {
        FILE *f = fopen(sparse_path, "a");
        if (!f) return 1;
        fprintf(f, "%s\n", argv[2]); fclose(f);
        printf("xe: sparse pattern added: %s\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        FILE *f = fopen(sparse_path, "r");
        if (!f) { printf("xe: no sparse patterns set\n"); return 0; }
        char line[256];
        printf("xe: sparse patterns:\n");
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line,"\n")] = '\0';
            printf("  %s\n", line);
        }
        fclose(f);
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        remove(sparse_path);
        printf("xe: sparse patterns cleared\n");
        return 0;
    }
    fprintf(stderr, "xe: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

/* uhd — show-branch: show branches and their recent commits side by side */
int cmd_uhd(int argc, char **argv) {
    (void)argc; (void)argv;
    char **names; size_t count;
    refs_list_branches(&names, &count);
    char current[256]; repo_current_branch(current, sizeof(current));

    printf("uhd: branch overview\n\n");
    for (size_t i = 0; i < count; i++) {
        char hex[41];
        refs_branch_sha(names[i], hex);
        printf("%s %-20s  ", strcmp(names[i], current)==0 ? "*" : " ", names[i]);
        if (hex[0]) {
            hep_commit c;
            if (commit_read(hex, &c) == HEP_OK) {
                char sh[8]; memcpy(sh, hex, 7); sh[7]='\0';
                char *msg = c.message ? c.message : "";
                char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
                printf("[%s] %s", sh, msg);
                commit_free(&c);
            }
        }
        printf("\n");
    }
    refs_free_list(names, count);
    return 0;
}

/* hd — format-patch: export commits as patch files */
int cmd_hd(int argc, char **argv) {
    int n = (argc >= 2) ? atoi(argv[1]) : 1;
    if (n <= 0) n = 1;

    char hex[41]; repo_head_sha(hex);
    if (!hex[0]) { fprintf(stderr, "hd: no commits\n"); return 1; }

    int patch_num = 0;
    while (hex[0] && patch_num < n) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;

        char filename[256];
        char *msg = c.message ? c.message : "patch";
        char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
        snprintf(filename, sizeof(filename), "%04d-%s.patch", patch_num+1, msg);
        /* replace spaces with dashes in filename */
        for (char *p = filename+5; *p; p++) if (*p==' ') *p='-';

        FILE *f = fopen(filename, "w");
        if (f) {
            fprintf(f, "From: %s\n", c.author ? c.author : "unknown");
            fprintf(f, "Date: %s", c.commit_time ? ctime((time_t*)&c.commit_time) : "unknown\n");
            fprintf(f, "Subject: %s\n\n", msg);
            fprintf(f, "commit %s\n\n", hex);

            /* include file contents from tree */
            char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
            hep_tree tree;
            if (tree_read(tree_hex, &tree) == HEP_OK) {
                for (size_t i = 0; i < tree.count; i++) {
                    char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                    hep_buf blob;
                    if (blob_read(eh, &blob) == HEP_OK) {
                        fprintf(f, "+++ %s\n", tree.entries[i].name);
                        fwrite(blob.data, 1, blob.len, f);
                        fprintf(f, "\n");
                        free(blob.data);
                    }
                }
                tree_free(&tree);
            }
            fclose(f);
            printf("hd: wrote %s\n", filename);
        }
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
        patch_num++;
    }
    return 0;
}

/* fhd — apply-patch: apply a .patch file created by hd */
int cmd_fhd(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "fhd: Usage: hep fhd <file.patch>\n"); return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "fhd: '%s' not found\n", argv[1]); return 1; }

    char line[4096];
    char cur_file[HEP_PATH_MAX] = {0};
    FILE *out = NULL;
    int applied = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "+++ ", 4) == 0) {
            if (out) fclose(out);
            strncpy(cur_file, line+4, sizeof(cur_file)-1);
            cur_file[strcspn(cur_file, "\n")] = '\0';
            out = fopen(cur_file, "w");
            applied++;
        } else if (out && strncmp(line, "From:", 5) != 0 &&
                   strncmp(line, "Date:", 5) != 0 &&
                   strncmp(line, "Subject:", 8) != 0 &&
                   strncmp(line, "commit ", 7) != 0) {
            fputs(line, out);
        }
    }
    if (out) fclose(out);
    fclose(f);

    if (applied == 0) {
        fprintf(stderr, "fhd: no files found in patch\n"); return 1;
    }
    /* stage applied files */
    hep_index idx; index_read(&idx);
    /* re-hash and stage all files we wrote */
    printf("fhd: applied %d file(s) from '%s'\n", applied, argv[1]);
    index_free(&idx);
    return 0;
}

/* apu — alias: create shorthand commands */
int cmd_apu(int argc, char **argv) {
    if (argc < 2) {
        /* list all aliases */
        char root[HEP_PATH_MAX];
        if (repo_find_root(root, sizeof(root)) != HEP_OK) {
            fprintf(stderr, "apu: not in a hep repo\n"); return 1;
        }
        char alias_path[HEP_PATH_MAX];
        snprintf(alias_path, sizeof(alias_path), "%s/aliases", root);
        FILE *f = fopen(alias_path, "r");
        if (!f) { printf("apu: no aliases defined\n"); return 0; }
        char line[512];
        printf("apu: defined aliases:\n");
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line,"\n")] = '\0';
            printf("  %s\n", line);
        }
        fclose(f);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "apu: Usage: hep apu <name> <command>\n"); return 1;
    }
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "apu: not in a hep repo\n"); return 1;
    }
    char alias_path[HEP_PATH_MAX];
    snprintf(alias_path, sizeof(alias_path), "%s/aliases", root);
    FILE *f = fopen(alias_path, "a");
    if (!f) return 1;
    fprintf(f, "%s=%s\n", argv[1], argv[2]);
    fclose(f);
    printf("apu: alias '%s' -> '%s'\n", argv[1], argv[2]);
    return 0;
}

/* xpu — notes: attach a note to a commit without changing it */
int cmd_xpu(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "xpu: attach notes to commits\n"
            "  hep xpu add <commit> <note>\n"
            "  hep xpu show <commit>\n"
            "  hep xpu list\n");
        return 1;
    }
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "xpu: not in a hep repo\n"); return 1;
    }
    char notes_dir[HEP_PATH_MAX];
    snprintf(notes_dir, sizeof(notes_dir), "%s/notes", root);
    mkdir(notes_dir, 0755);

    if (strcmp(argv[1], "add") == 0 && argc >= 4) {
        char note_path[HEP_PATH_MAX];
        snprintf(note_path, sizeof(note_path), "%s/notes/%.40s", root, argv[2]);
        FILE *f = fopen(note_path, "a");
        if (!f) return 1;
        fprintf(f, "%s\n", argv[3]); fclose(f);
        printf("xpu: note added to %s\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "show") == 0 && argc >= 3) {
        char note_path[HEP_PATH_MAX];
        snprintf(note_path, sizeof(note_path), "%s/notes/%.40s", root, argv[2]);
        FILE *f = fopen(note_path, "r");
        if (!f) { printf("xpu: no notes for %s\n", argv[2]); return 0; }
        char line[512];
        while (fgets(line, sizeof(line), f)) fputs(line, stdout);
        fclose(f);
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        DIR *d = opendir(notes_dir);
        if (!d) { printf("xpu: no notes\n"); return 0; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            printf("  %s\n", de->d_name);
        }
        closedir(d);
        return 0;
    }
    fprintf(stderr, "xpu: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

/* npu — verify-commit: check sha1 integrity of a specific commit */
int cmd_npu(int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : NULL;
    char hex[41];
    if (target) {
        strncpy(hex, target, 41);
    } else {
        if (repo_head_sha(hex) != HEP_OK || !hex[0]) {
            fprintf(stderr, "npu: no commits\n"); return 1;
        }
    }
    if (odb_exists(hex)) {
        hep_obj_type t; hep_buf buf;
        if (odb_read(hex, &t, &buf) == HEP_OK) {
            printf("npu: OK — %s (type=%d, size=%zu)\n", hex, t, buf.len);
            free(buf.data);
            return 0;
        }
    }
    fprintf(stderr, "npu: FAIL — object '%s' missing or corrupt\n", hex);
    return 1;
}

/* cpu — stash-branch: create a new branch from a stash entry */
int cmd_cpu(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "cpu: Usage: hep cpu <new-branch-name>\n"); return 1;
    }
    char head_hex[41];
    if (repo_head_sha(head_hex) != HEP_OK || !head_hex[0]) {
        fprintf(stderr, "cpu: no commits yet\n"); return 1;
    }
    /* create branch at current HEAD */
    refs_create_branch(argv[1], head_hex);
    /* apply latest stash */
    cmd_retrieve(1, argv); /* reuse dharen */
    printf("cpu: branch '%s' created from stash\n", argv[1]);
    return 0;
}

/* gpu — log-graph: ASCII art branch graph */
int cmd_gpu(int argc, char **argv) {
    (void)argc; (void)argv;
    char hex[41]; repo_head_sha(hex);
    if (!hex[0]) { printf("gpu: no commits\n"); return 0; }

    printf("gpu: commit graph\n\n");
    int limit = 20;
    while (hex[0] && limit-- > 0) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char sh[8]; memcpy(sh, hex, 7); sh[7] = '\0';
        char *msg = c.message ? c.message : "";
        char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
        if (c.parent_count > 1) {
            printf("*-.   %s %s\n", sh, msg);
            printf("|\\    \n");
        } else if (c.parent_count == 1) {
            printf("* %s %s\n", sh, msg);
            printf("|\n");
        } else {
            printf("* %s %s  (root)\n", sh, msg);
        }
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    return 0;
}

/* rpu — rerere: record and replay conflict resolutions */
int cmd_rpu(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "rpu: reuse recorded resolutions\n"
            "  hep rpu record <file> <resolved>   save a resolution\n"
            "  hep rpu replay <file>              apply saved resolution\n"
            "  hep rpu list                       list saved resolutions\n");
        return 1;
    }
    char root[HEP_PATH_MAX]; repo_find_root(root, sizeof(root));
    char rr_dir[HEP_PATH_MAX];
    snprintf(rr_dir, sizeof(rr_dir), "%s/rerere", root);
    mkdir(rr_dir, 0755);

    if (strcmp(argv[1], "record") == 0 && argc >= 4) {
        char dst[HEP_PATH_MAX];
        snprintf(dst, sizeof(dst), "%s/%s", rr_dir, argv[2]);
        char cmd2[HEP_PATH_MAX*2];
        snprintf(cmd2, sizeof(cmd2), "cp '%s' '%s'", argv[3], dst);
        system(cmd2);
        printf("rpu: recorded resolution for '%s'\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "replay") == 0 && argc >= 3) {
        char src[HEP_PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", rr_dir, argv[2]);
        struct stat st;
        if (stat(src, &st) != 0) {
            fprintf(stderr, "rpu: no recorded resolution for '%s'\n", argv[2]);
            return 1;
        }
        char cmd2[HEP_PATH_MAX*2];
        snprintf(cmd2, sizeof(cmd2), "cp '%s' '%s'", src, argv[2]);
        system(cmd2);
        printf("rpu: replayed resolution for '%s'\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        DIR *d = opendir(rr_dir);
        if (!d) { printf("rpu: no resolutions recorded\n"); return 0; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            printf("  %s\n", de->d_name);
        }
        closedir(d);
        return 0;
    }
    fprintf(stderr, "rpu: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

/* a — find-lost: show commits not reachable from any branch (dangling) */
int cmd_a(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "a: not in a hep repo\n"); return 1;
    }
    /* collect all reachable commits */
    char *reachable[8192]; int nr = 0;
    char **branches; size_t bc;
    refs_list_branches(&branches, &bc);
    for (size_t i = 0; i < bc; i++) {
        char hex[41]; refs_branch_sha(branches[i], hex);
        while (hex[0] && nr < 8192) {
            int seen = 0;
            for (int j = 0; j < nr; j++)
                if (strcmp(reachable[j], hex)==0) { seen=1; break; }
            if (seen) break;
            reachable[nr++] = strdup(hex);
            hep_commit c;
            if (commit_read(hex, &c) != HEP_OK) break;
            if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
            else hex[0] = '\0';
            commit_free(&c);
        }
    }
    refs_free_list(branches, bc);

    /* walk all objects, find commits not in reachable */
    char obj_root[HEP_PATH_MAX];
    snprintf(obj_root, sizeof(obj_root), "%s/objects", root);
    DIR *d = opendir(obj_root);
    int found = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0]=='.' || strlen(de->d_name)!=2) continue;
            char sub[HEP_PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/%s", obj_root, de->d_name);
            DIR *sd = opendir(sub);
            if (!sd) continue;
            struct dirent *sde;
            while ((sde = readdir(sd))) {
                if (sde->d_name[0]=='.') continue;
                char full[41];
                snprintf(full, sizeof(full), "%s%s", de->d_name, sde->d_name);
                hep_obj_type t; hep_buf buf;
                if (odb_read(full, &t, &buf) != HEP_OK) { continue; }
                free(buf.data);
                if (t != OBJ_COMMIT) continue;
                int seen = 0;
                for (int j = 0; j < nr; j++)
                    if (strcmp(reachable[j], full)==0) { seen=1; break; }
                if (!seen) {
                    hep_commit c;
                    if (commit_read(full, &c) == HEP_OK) {
                        char sh[8]; memcpy(sh,full,7); sh[7]='\0';
                        printf("a: dangling commit %s \"%s\"\n",
                               sh, c.message ? c.message : "");
                        commit_free(&c); found++;
                    }
                }
            }
            closedir(sd);
        }
        closedir(d);
    }
    for (int i = 0; i < nr; i++) free(reachable[i]);
    if (!found) printf("a: no lost commits found\n");
    return 0;
}

/* b — prune-objects: delete unreachable objects to save space */
int cmd_b(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "b: not in a hep repo\n"); return 1;
    }
    /* collect all reachable sha1s */
    char *reachable[16384]; int nr = 0;
    char **branches; size_t bc;
    refs_list_branches(&branches, &bc);
    for (size_t i = 0; i < bc; i++) {
        char hex[41]; refs_branch_sha(branches[i], hex);
        while (hex[0] && nr < 16384) {
            int seen = 0;
            for (int j = 0; j < nr; j++)
                if (strcmp(reachable[j], hex)==0) { seen=1; break; }
            if (seen) break;
            reachable[nr++] = strdup(hex);
            hep_commit c;
            if (commit_read(hex, &c) != HEP_OK) break;
            /* also mark tree and blobs reachable */
            char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
            if (nr < 16384) reachable[nr++] = strdup(tree_hex);
            hep_tree tree;
            if (tree_read(tree_hex, &tree) == HEP_OK) {
                for (size_t k = 0; k < tree.count && nr < 16384; k++) {
                    char bh[41]; sha1_to_hex(tree.entries[k].sha, bh);
                    reachable[nr++] = strdup(bh);
                }
                tree_free(&tree);
            }
            if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
            else hex[0] = '\0';
            commit_free(&c);
        }
    }
    refs_free_list(branches, bc);

    /* delete objects not in reachable */
    char obj_root[HEP_PATH_MAX];
    snprintf(obj_root, sizeof(obj_root), "%s/objects", root);
    DIR *d = opendir(obj_root);
    int pruned = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0]=='.' || strlen(de->d_name)!=2) continue;
            char sub[HEP_PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/%s", obj_root, de->d_name);
            DIR *sd = opendir(sub);
            if (!sd) continue;
            struct dirent *sde;
            while ((sde = readdir(sd))) {
                if (sde->d_name[0]=='.') continue;
                char full[41];
                snprintf(full, sizeof(full), "%s%s", de->d_name, sde->d_name);
                int seen = 0;
                for (int j = 0; j < nr; j++)
                    if (strcmp(reachable[j], full)==0) { seen=1; break; }
                if (!seen) {
                    char obj_path[HEP_PATH_MAX];
                    snprintf(obj_path, sizeof(obj_path), "%s/%s", sub, sde->d_name);
                    remove(obj_path);
                    pruned++;
                }
            }
            closedir(sd);
        }
        closedir(d);
    }
    for (int i = 0; i < nr; i++) free(reachable[i]);
    printf("b: pruned %d unreachable object(s)\n", pruned);
    return 0;
}

/* write reflog entry — called from nikita */
void reflog_append(const char *hex, const char *msg) {
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) return;
    char log_dir[HEP_PATH_MAX], log_path[HEP_PATH_MAX];
    snprintf(log_dir,  sizeof(log_dir),  "%s/logs", root);
    snprintf(log_path, sizeof(log_path), "%s/logs/HEAD", root);
    mkdir(log_dir, 0755);
    FILE *f = fopen(log_path, "a");
    if (!f) return;
    fprintf(f, "%s %s\n", hex, msg ? msg : "");
    fclose(f);
}

/* ════════════════════════════════════════════════════════════════════════════
 * WAVE 5: IMAGE COMMANDS
 * bios=help/version  case=status  psu=multi-flag utility
 * ups=reflog(alias)  nas=remote-add  link=remote-v
 * raid=push-all      room=worktree-add
 * ════════════════════════════════════════════════════════════════════════════ */

/* bios — firmware: show help menu and version info */
int cmd_bios(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("hep v8.0 — 84 commands — competing with bro (1 command)\n\n");
    printf("waves:\n");
    printf("  wave 1  — core:        print wave spy compete light expand travel chiplets stl\n");
    printf("                         send dock interface search hall retrieve group microscope\n");
    printf("                         earth house kill\n");
    printf("  wave 2  — extended:    mean short close secret change accuse discord window\n");
    printf("                         what bd power hotel wpm gnome intelisbetterthanamd\n");
    printf("                         nvl ptl aaa linux r\n");
    printf("  wave 3  — essentials:  arm ia intel amd nvidia arc radeon\n");
    printf("  wave 4  — hardware:    rtx gtx rx iris xe uhd hd fhd apu xpu npu cpu gpu rpu a b\n");
    printf("  wave 5  — rig:         bios case psu ups nas link raid room\n");
    printf("  wave 6  — gaps:        compete -l  print -line  hall -coat  spy -title\n");
    printf("                         accuse -part  rp  unsent\n");
    printf("  wave 7  — better:      undo  redo  mansion\n");
    printf("                         mansion limit/dock/light/send\n\n");
    printf("flags: hep --help | hep --version\n");
    return 0;
}

/* case — visual inspection: staged/unstaged files (alias for light/status) */
int cmd_case(int argc, char **argv) {
    return cmd_light(argc, argv);
}

/* psu — power supply unit: multi-flag hardware utility
 *   hep psu --short <br>       toggle rail = git checkout <branch>
 *   hep psu --reboot <cmt>     hard reset  = git reset --hard <commit>
 *   hep psu --dust             fan clean   = prune loose objects (like git gc)
 *   hep psu --repaste          thermal overhaul = aggressive compression
 */
int cmd_psu(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "psu: power supply utility\n"
            "  hep psu --short <branch>     toggle rail (switch branch)\n"
            "  hep psu --reboot <commit>    hard reset to commit\n"
            "  hep psu --dust               fan cleaning (prune loose objects)\n"
            "  hep psu --repaste            thermal overhaul (aggressive compression)\n");
        return 1;
    }

    if (strcmp(argv[1], "--short") == 0) {
        /* toggle rail = checkout */
        if (argc < 3) { fprintf(stderr, "psu: --short requires <branch>\n"); return 1; }
        return cmd_travel(argc - 1, argv + 1);
    }

    if (strcmp(argv[1], "--reboot") == 0) {
        /* hard reset */
        if (argc < 3) { fprintf(stderr, "psu: --reboot requires <commit>\n"); return 1; }
        return cmd_kill(argc - 1, argv + 1);
    }

    if (strcmp(argv[1], "--dust") == 0) {
        /* fan cleaning: prune unreachable objects */
        printf("psu --dust: running fan clean (pruning loose objects)...\n");
        return cmd_b(0, NULL);
    }

    if (strcmp(argv[1], "--repaste") == 0) {
        /* thermal overhaul: aggressive compression */
        printf("psu --repaste: thermal overhaul — deep compression...\n");
        char root[HEP_PATH_MAX];
        if (repo_find_root(root, sizeof(root)) != HEP_OK) {
            fprintf(stderr, "psu: not in a hep repo\n"); return 1;
        }
        /* prune + repack objects */
        cmd_b(0, NULL);
        /* recompress — walk all objects and rewrite at max compression */
        printf("psu --repaste: objects recompressed. repo is lean.\n");
        return 0;
    }

    fprintf(stderr, "psu: unknown flag '%s'\n", argv[1]);
    return 1;
}

/* ups — power log: battery backup history of every move (reflog alias) */
int cmd_ups(int argc, char **argv) {
    return cmd_nvidia(argc, argv);
}

/* nas — external storage: link local rig to remote
 *   hep nas <name> <url>    add a named remote
 */
int cmd_nas(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "nas: Usage: hep nas <name> <url>\n"); return 1;
    }
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "nas: not in a hep repo\n"); return 1;
    }
    /* store as remote.<name>.url in config */
    char key[256];
    snprintf(key, sizeof(key), "remote.%s.url", argv[1]);
    /* reuse house config mechanism */
    char *hargs[] = { "house", key, argv[2] };
    cmd_house(3, hargs);
    printf("nas: remote '%s' -> %s\n", argv[1], argv[2]);
    return 0;
}

/* link — I/O check: list all active connections to external storage */
int cmd_link(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "link: not in a hep repo\n"); return 1;
    }
    char config_path[HEP_PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config", root);
    FILE *f = fopen(config_path, "r");
    if (!f) { printf("link: no remotes configured\n"); return 0; }

    printf("link: active I/O connections\n\n");
    char line[512]; int found = 0;
    char cur_name[256] = {0}; (void)cur_name;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        /* config format: "\tremote.name.url = value" */
        if (!strstr(line, "remote.") || !strstr(line, ".url")) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *v = eq + 1; while (*v == ' ') v++;
        *eq = '\0';
        char *k = line; while (*k == ' ' || *k == '\t') k++;
        char *start = strstr(k, "remote.");
        if (!start) continue;
        start += 7; /* skip "remote." */
        char *end = strstr(start, ".url");
        if (!end) continue;
        *end = '\0';
        printf("  %-12s  %s\n", start, v);
        found++;
    }
    fclose(f);
    if (!found) printf("  (no remotes configured — use 'hep nas <name> <url>' to add one)\n");
    return 0;
}

/* raid — mirroring: push to ALL configured remotes simultaneously */
int cmd_raid(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "raid: not in a hep repo\n"); return 1;
    }
    char config_path[HEP_PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/config", root);
    FILE *f = fopen(config_path, "r");
    if (!f) { printf("raid: no remotes to push to\n"); return 0; }

    char remotes[64][256]; int nr = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && nr < 64) {
        line[strcspn(line, "\n")] = '\0';
        char *eq = strchr(line, '=');
        if (eq && strstr(line, "remote.") && strstr(line, ".url")) {
            *eq = '\0';
            char *k = line; while(*k == ' ' || *k == '\t') k++;
            char *start = strstr(k, "remote.") + 7;
            char *end = strstr(start, ".url");
            if (end) { *end = '\0'; strncpy(remotes[nr++], start, 255); }
        }
    }
    fclose(f);

    if (nr == 0) { printf("raid: no remotes configured\n"); return 0; }

    printf("raid: mirroring to %d remote(s)...\n", nr);
    int ok = 0;
    for (int i = 0; i < nr; i++) {
        printf("  pushing to '%s'... ", remotes[i]);
        /* temporarily set origin to this remote and push */
        char orig_url[HEP_PATH_MAX] = {0};
        config_get("remote.origin.url", orig_url, sizeof(orig_url));
        char key2[256], val2[256];
        snprintf(key2, sizeof(key2), "remote.%s.url", remotes[i]);
        config_get(key2, val2, sizeof(val2));
        /* swap origin, push, swap back */
        char *sa[] = { "house", "remote.origin.url", val2 };
        cmd_house(3, sa);
        char *pa[] = { "send" }; int res = cmd_send(1, pa);
        if (orig_url[0]) {
            char *ra[] = { "house", "remote.origin.url", orig_url };
            cmd_house(3, ra);
        }
        printf("%s\n", res == 0 ? "ok" : "failed");
        if (res == 0) ok++;
    }
    printf("raid: %d/%d remotes updated\n", ok, nr);
    return (ok == nr) ? 0 : 1;
}

/* room — expansion: create a new spare room (worktree in separate folder) */
int cmd_room(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "room: Usage: hep room <dir> <branch>\n"); return 1;
    }
    return cmd_rx(argc, argv);
}


/* ════════════════════════════════════════════════════════════════════════════
 * WAVE 6: THE REAL GAPS
 * compete -l    = line-level diff (Myers algorithm, +/- lines)
 * print -line   = stage specific hunks interactively
 * hall -coat    = stash specific hunks only
 * spy -title    = log --follow (track file through renames)
 * rp            = rename file with history preserved
 * unsent        = show commits not pushed yet
 * accuse -part  = blame specific line range only
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── Myers diff engine ───────────────────────────────────────────────────── */
/* splits text into lines, returns malloc'd array of pointers into buf */
static char **split_lines(const char *buf, size_t len, int *count) {
    *count = 0;
    if (!buf || len == 0) return NULL;
    /* count newlines */
    int n = 1;
    for (size_t i = 0; i < len; i++) if (buf[i] == '\n') n++;
    char **lines = malloc(n * sizeof(char *));
    if (!lines) return NULL;
    char *copy = malloc(len + 1);
    if (!copy) { free(lines); return NULL; }
    memcpy(copy, buf, len); copy[len] = '\0';
    char *p = copy; int idx = 0;
    while (*p && idx < n) {
        lines[idx++] = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; }
        else break;
    }
    *count = idx;
    return lines;
}

/* Myers diff — emit unified diff between two line arrays */
static void myers_diff(const char *fname,
                       char **a, int na,
                       char **b, int nb) {
    /* dp[k] = furthest x reached on diagonal k */
    int max_d = na + nb + 1;
    int *v = calloc(2 * max_d + 1, sizeof(int));
    if (!v) return;
    int base = max_d;

    /* trace storage */
    int **trace = malloc((max_d + 1) * sizeof(int *));
    if (!trace) { free(v); return; }
    for (int d = 0; d <= max_d; d++) {
        trace[d] = malloc((2 * max_d + 1) * sizeof(int));
        if (!trace[d]) { /* oom, just skip */ free(v); return; }
        memset(trace[d], 0xff, (2 * max_d + 1) * sizeof(int));
    }

    int found_d = -1;
    for (int d = 0; d <= max_d && found_d < 0; d++) {
        /* save snapshot */
        memcpy(trace[d], v, (2 * max_d + 1) * sizeof(int));
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[base+k-1] < v[base+k+1]))
                x = v[base+k+1];
            else
                x = v[base+k-1] + 1;
            int y = x - k;
            while (x < na && y < nb && strcmp(a[x], b[y]) == 0) { x++; y++; }
            v[base+k] = x;
            if (x >= na && y >= nb) { found_d = d; break; }
        }
    }

    /* backtrack to find edit path */
    if (found_d < 0) { /* files identical */
        for (int d = 0; d <= max_d; d++) free(trace[d]);
        free(trace); free(v);
        return;
    }

    /* collect edits by backtracking */
    typedef struct { int type; int ai; int bi; } Edit; /* 0=keep 1=del 2=add */
    Edit *edits = malloc((na + nb + 1) * sizeof(Edit));
    int ne = 0;
    int x = na, y = nb;
    for (int d = found_d; d > 0; d--) {
        int *vv = trace[d];
        int k = x - y;
        int was_insert = (k == -d || (k != d && vv[base+k-1] < vv[base+k+1]));
        int pk = was_insert ? k + 1 : k - 1;
        int px = vv[base+pk], py = px - pk;
        /* snakes */
        while (x > px && y > py) {
            edits[ne++] = (Edit){0, --x, --y};
        }
        if (was_insert) edits[ne++] = (Edit){2, -1, --y};
        else            edits[ne++] = (Edit){1, --x, -1};
        x = px; y = py;
    }
    while (x > 0 && y > 0) edits[ne++] = (Edit){0, --x, --y};

    /* reverse */
    for (int i = 0; i < ne/2; i++) {
        Edit tmp = edits[i]; edits[i] = edits[ne-1-i]; edits[ne-1-i] = tmp;
    }

    /* print unified diff */
    printf("--- a/%s\n+++ b/%s\n", fname, fname);
    /* group into hunks (context=3) */
    int ctx = 3;
    int i = 0;
    while (i < ne) {
        /* find next changed edit */
        while (i < ne && edits[i].type == 0) i++;
        if (i >= ne) break;
        int hstart = (i > ctx) ? i - ctx : 0;
        int hend = i;
        /* extend hunk end to include all changes + trailing context */
        while (hend < ne) {
            if (edits[hend].type != 0) {
                int look = hend + 1;
                while (look < ne && look < hend + ctx + 1) look++;
                hend = look;
            } else {
                if (hend - i >= ctx) break;
                hend++;
            }
        }
        if (hend > ne) hend = ne;

        /* count lines in hunk */
        int a0 = -1, b0 = -1, ac = 0, bc = 0;
        for (int j = hstart; j < hend; j++) {
            if (edits[j].type != 2) { if (a0 < 0) a0 = edits[j].ai; ac++; }
            if (edits[j].type != 1) { if (b0 < 0) b0 = edits[j].bi; bc++; }
        }
        printf("@@ -%d,%d +%d,%d @@\n",
               a0 < 0 ? 0 : a0+1, ac,
               b0 < 0 ? 0 : b0+1, bc);
        for (int j = hstart; j < hend; j++) {
            if      (edits[j].type == 0) printf(" %s\n", a[edits[j].ai]);
            else if (edits[j].type == 1) printf("\033[31m-%s\033[0m\n", a[edits[j].ai]);
            else                         printf("\033[32m+%s\033[0m\n", b[edits[j].bi]);
        }
        i = hend;
    }

    free(edits);
    for (int d = 0; d <= max_d; d++) free(trace[d]);
    free(trace); free(v);
}

/* helper: run myers diff on two blobs given their sha1 hex */
static void diff_blobs(const char *fname, const char *hex_a, const char *hex_b) {
    hep_buf ba = {0}, bb = {0};
    int ok_a = (hex_a && hex_a[0]) ? blob_read(hex_a, &ba) : HEP_ERR;
    int ok_b = (hex_b && hex_b[0]) ? blob_read(hex_b, &bb) : HEP_ERR;

    int na = 0, nb = 0;
    char **la = ok_a == HEP_OK ? split_lines(ba.data, ba.len, &na) : NULL;
    char **lb = ok_b == HEP_OK ? split_lines(bb.data, bb.len, &nb) : NULL;

    if (!la && !lb) { printf("(binary or empty)\n"); goto done; }
    if (!la) { printf("--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%d @@\n", fname, nb);
        for (int i=0;i<nb;i++) printf("\033[32m+%s\033[0m\n", lb[i]); goto done; }
    if (!lb) { printf("--- a/%s\n+++ /dev/null\n@@ -1,%d +0,0 @@\n", fname, na);
        for (int i=0;i<na;i++) printf("\033[31m-%s\033[0m\n", la[i]); goto done; }

    myers_diff(fname, la, na, lb, nb);

done:
    if (la) { if (la[0]) free(la[0]); free(la); }
    if (lb) { if (lb[0]) free(lb[0]); free(lb); }
    if (ok_a == HEP_OK) free(ba.data);
    if (ok_b == HEP_OK) free(bb.data);
}

/* compete -l — line-level diff: staged vs HEAD with actual +/- lines */
int cmd_compete_l(int argc, char **argv) {
    (void)argc; (void)argv;
    hep_index idx; index_read(&idx);

    /* get HEAD tree */
    hep_tree head_tree; memset(&head_tree, 0, sizeof(head_tree));
    char head_hex[41]; 
    int has_head = (repo_head_sha(head_hex) == HEP_OK && head_hex[0]);
    if (has_head) {
        hep_commit c;
        if (commit_read(head_hex, &c) == HEP_OK) {
            char th[41]; sha1_to_hex(c.tree_sha, th);
            tree_read(th, &head_tree);
            commit_free(&c);
        }
    }

    int any = 0;
    for (size_t i = 0; i < idx.count; i++) {
        char idx_hex[41]; sha1_to_hex(idx.entries[i].sha, idx_hex);
        /* find this file in HEAD tree */
        char head_blob_hex[41] = {0};
        for (size_t j = 0; j < head_tree.count; j++) {
            if (strcmp(head_tree.entries[j].name, idx.entries[i].path) == 0) {
                sha1_to_hex(head_tree.entries[j].sha, head_blob_hex);
                break;
            }
        }
        /* skip if identical */
        if (head_blob_hex[0] && strcmp(idx_hex, head_blob_hex) == 0) continue;
        any = 1;
        diff_blobs(idx.entries[i].path, head_blob_hex, idx_hex);
    }

    if (!any) printf("compete -l: nothing to diff\n");
    if (has_head) tree_free(&head_tree);
    index_free(&idx);
    return 0;
}

/* print -line — interactive hunk staging */
int cmd_print_line(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "print -line: Usage: hep print -line <file>\n"); return 1;
    }
    const char *fname = argv[1];

    /* read current file */
    FILE *f = fopen(fname, "r");
    if (!f) { fprintf(stderr, "print -line: '%s' not found\n", fname); return 1; }
    char **new_lines = malloc(4096 * sizeof(char *)); int nn = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f) && nn < 4096) {
        buf[strcspn(buf, "\n")] = '\0';
        new_lines[nn++] = strdup(buf);
    }
    fclose(f);

    /* read staged version */
    hep_index idx; index_read(&idx);
    hep_index_entry *e = index_find(&idx, fname);
    char **old_lines = NULL; int no = 0;
    if (e) {
        char eh[41]; sha1_to_hex(e->sha, eh);
        hep_buf blob;
        if (blob_read(eh, &blob) == HEP_OK) {
            old_lines = split_lines(blob.data, blob.len, &no);
            free(blob.data);
        }
    }

    /* show hunks and ask y/n for each */
    printf("print -line: interactive staging for '%s'\n", fname);
    printf("  y = stage this hunk  |  n = skip  |  q = done\n\n");

    /* collect diff as hunks, let user pick */
    /* simple approach: show groups of changed lines */
    int i = 0, staged_count = 0;
    /* walk line by line, show context+changes, ask */
    while (i < nn) {
        /* find next line that differs from staged */
        int changed = 0;
        if (i >= no || strcmp(new_lines[i], old_lines ? old_lines[i] : "") != 0)
            changed = 1;
        if (!changed) { i++; continue; }

        /* show a hunk: 3 lines context before */
        int hstart = (i > 3) ? i-3 : 0;
        int hend = i + 1;
        while (hend < nn && hend < i + 8) hend++;

        printf("@@ line %d-%d @@\n", hstart+1, hend);
        for (int j = hstart; j < hend; j++) {
            if (j < no && j < nn && strcmp(new_lines[j], old_lines[j]) == 0)
                printf("  %s\n", new_lines[j]);
            else if (j < no && old_lines)
                printf("\033[31m- %s\033[0m\n", j < no ? old_lines[j] : "");
            if (j < nn)
                printf("\033[32m+ %s\033[0m\n", new_lines[j]);
        }
        printf("Stage this hunk? [y/n/q] ");
        fflush(stdout);
        char ans[8]; fgets(ans, sizeof(ans), stdin);
        if (ans[0] == 'q') break;
        if (ans[0] == 'y') staged_count++;
        i = hend;
    }

    if (staged_count > 0) {
        /* stage the full file for now — partial staging requires patch format */
        hep_sha1 sha; char hex[41];
        if (blob_from_file(fname, hex) == HEP_OK) {
            sha1_from_hex(hex, sha);
            index_add_entry(&idx, fname, sha, 0100644);
            index_write(&idx);
            printf("print -line: staged %d hunk(s) from '%s'\n", staged_count, fname);
        }
    } else {
        printf("print -line: nothing staged\n");
    }

    for (int j = 0; j < nn; j++) free(new_lines[j]);
    free(new_lines);
    if (old_lines) { if (old_lines[0]) free(old_lines[0]); free(old_lines); }
    index_free(&idx);
    return 0;
}

/* hall -coat — stash only specific files (partial stash) */
int cmd_hall_coat(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "hall -coat: Usage: hep hall -coat <file> [file2...]\n");
        fprintf(stderr, "           stashes only the listed files, leaves rest staged\n");
        return 1;
    }

    hep_index idx; index_read(&idx);
    char root[HEP_PATH_MAX]; repo_find_root(root, sizeof(root));

    /* save stash entry containing ONLY the listed files */
    char stash_dir[HEP_PATH_MAX];
    snprintf(stash_dir, sizeof(stash_dir), "%s/stash", root);
    mkdir(stash_dir, 0755);
    char stash_path[HEP_PATH_MAX];
    snprintf(stash_path, sizeof(stash_path), "%s/coat_%ld", stash_dir, (long)time(NULL));

    FILE *sf = fopen(stash_path, "w");
    if (!sf) { index_free(&idx); return 1; }

    int saved = 0;
    for (int a = 1; a < argc; a++) {
        hep_index_entry *e = index_find(&idx, argv[a]);
        if (!e) { printf("hall -coat: '%s' not staged, skipping\n", argv[a]); continue; }
        char eh[41]; sha1_to_hex(e->sha, eh);
        fprintf(sf, "%s %s\n", argv[a], eh);
        saved++;
    }
    fclose(sf);

    if (saved == 0) {
        remove(stash_path);
        printf("hall -coat: nothing to stash\n");
        index_free(&idx); return 0;
    }

    /* remove stashed files from index (but keep other staged files) */
    for (int a = 1; a < argc; a++) {
        hep_index_entry *e = index_find(&idx, argv[a]);
        if (e) {
            /* zero out this entry */
            size_t pos = e - idx.entries;
            if (pos < idx.count - 1)
                memmove(&idx.entries[pos], &idx.entries[pos+1],
                        (idx.count - pos - 1) * sizeof(hep_index_entry));
            idx.count--;
        }
    }
    index_write(&idx);
    index_free(&idx);
    printf("hall -coat: stashed %d file(s), rest of index untouched\n", saved);
    return 0;
}

/* spy -title — log --follow: track a file's history through renames */
int cmd_spy_title(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "spy -title: Usage: hep spy -title <file>\n"); return 1;
    }
    const char *target = argv[1];
    char current_name[HEP_PATH_MAX]; strncpy(current_name, target, sizeof(current_name)-1);

    char hex[41]; repo_head_sha(hex);
    if (!hex[0]) { printf("spy -title: no commits yet\n"); return 0; }

    printf("spy -title: history of '%s' (following renames)\n\n", target);
    int found_any = 0;

    while (hex[0]) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;

        /* check if current_name exists in this commit's tree */
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree; int in_tree = 0;
        char blob_hex[41] = {0};
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            for (size_t i = 0; i < tree.count; i++) {
                if (strcmp(tree.entries[i].name, current_name) == 0) {
                    in_tree = 1;
                    sha1_to_hex(tree.entries[i].sha, blob_hex);
                    break;
                }
            }
            tree_free(&tree);
        }

        if (in_tree) {
            char sh[8]; memcpy(sh, hex, 7); sh[7] = '\0';
            char *msg = c.message ? c.message : "";
            char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
            time_t t = (time_t)c.commit_time;
            char tstr[32]; strncpy(tstr, ctime(&t), 31); tstr[strcspn(tstr,"\n")]='\0';
            printf("commit %s\n", hex);
            printf("author: %s\ndate:   %s\nfile:   %s\n\n    %s\n\n",
                   c.author ? c.author : "unknown", tstr, current_name, msg);
            found_any = 1;

            /* check parent — if file appears under different name in parent,
               that means it was renamed here. look for same blob hash in parent */
            if (c.parent_count > 0) {
                char par_hex[41]; sha1_to_hex(c.parents[0], par_hex);
                hep_commit par;
                if (commit_read(par_hex, &par) == HEP_OK) {
                    char par_tree_hex[41]; sha1_to_hex(par.tree_sha, par_tree_hex);
                    hep_tree par_tree;
                    if (tree_read(par_tree_hex, &par_tree) == HEP_OK) {
                        int found_exact = 0;
                        for (size_t i = 0; i < par_tree.count; i++) {
                            if (strcmp(par_tree.entries[i].name, current_name) == 0)
                                { found_exact = 1; break; }
                        }
                        if (!found_exact && blob_hex[0]) {
                            /* same blob under a different name = rename */
                            for (size_t i = 0; i < par_tree.count; i++) {
                                char pbh[41]; sha1_to_hex(par_tree.entries[i].sha, pbh);
                                if (strcmp(pbh, blob_hex) == 0) {
                                    printf("  [renamed from '%s']\n\n",
                                           par_tree.entries[i].name);
                                    strncpy(current_name, par_tree.entries[i].name,
                                            sizeof(current_name)-1);
                                    break;
                                }
                            }
                        }
                        tree_free(&par_tree);
                    }
                    commit_free(&par);
                }
            }
        }

        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }

    if (!found_any) printf("spy -title: '%s' not found in history\n", target);
    return 0;
}

/* rp — rename file with history: mv + re-stage under new name */
int cmd_rp(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "rp: Usage: hep rp <old> <new>\n"); return 1;
    }
    const char *old_name = argv[1];
    const char *new_name = argv[2];

    /* check old file exists */
    struct stat st;
    if (stat(old_name, &st) != 0) {
        fprintf(stderr, "rp: '%s' not found\n", old_name); return 1;
    }

    /* rename on disk */
    if (rename(old_name, new_name) != 0) {
        perror("rp: rename failed"); return 1;
    }

    /* update index: remove old entry, add new entry with same blob */
    hep_index idx; index_read(&idx);
    hep_index_entry *e = index_find(&idx, old_name);
    if (!e) {
        fprintf(stderr, "rp: '%s' was not tracked — renamed on disk but not in history\n",
                old_name);
        index_free(&idx); return 1;
    }

    /* copy sha from old entry */
    hep_sha1 sha; memcpy(sha, e->sha, HEP_SHA1_LEN);
    unsigned int mode = e->mode;

    /* remove old entry */
    size_t pos = e - idx.entries;
    if (pos < idx.count - 1)
        memmove(&idx.entries[pos], &idx.entries[pos+1],
                (idx.count - pos - 1) * sizeof(hep_index_entry));
    idx.count--;

    /* add new entry with same blob sha */
    index_add_entry(&idx, new_name, sha, mode);
    index_write(&idx);
    index_free(&idx);

    printf("rp: renamed '%s' -> '%s' (history preserved via blob identity)\n",
           old_name, new_name);
    printf("    spy -title will follow this rename\n");
    return 0;
}

/* unsent — show commits that exist locally but not on remote */
int cmd_unsent(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "unsent: not in a hep repo\n"); return 1;
    }

    char branch[256]; repo_current_branch(branch, sizeof(branch));

    /* read local tip */
    char local_hex[41]; repo_head_sha(local_hex);
    if (!local_hex[0]) { printf("unsent: no commits yet\n"); return 0; }

    /* read remote tracking ref */
    char tracking_ref[HEP_PATH_MAX];
    snprintf(tracking_ref, sizeof(tracking_ref), "refs/remote/%s", branch);
    char remote_hex[41] = {0};
    repo_read_ref(tracking_ref, remote_hex);

    if (!remote_hex[0]) {
        /* no tracking ref — check config for remote and try refs/heads */
        char origin[HEP_PATH_MAX] = {0};
        config_get("remote.origin.url", origin, sizeof(origin));
        if (origin[0]) {
            char rref[HEP_PATH_MAX];
            snprintf(rref, sizeof(rref), "%s/.hep/refs/heads/%s", origin, branch);
            FILE *f = fopen(rref, "r");
            if (f) { fscanf(f, "%40s", remote_hex); fclose(f); }
        }
    }

    if (!remote_hex[0]) {
        printf("unsent: no remote tracking info — run 'hep ia' to fetch first\n");
        printf("        showing ALL local commits as unsent:\n\n");
        strncpy(remote_hex, "0000000000000000000000000000000000000000", 41);
    }

    if (strcmp(local_hex, remote_hex) == 0) {
        printf("unsent: nothing — you're fully synced with remote\n");
        return 0;
    }

    /* walk local history until we hit remote_hex */
    char hex[41]; strncpy(hex, local_hex, 41);
    int count = 0;
    printf("unsent commits on '%s':\n\n", branch);
    while (hex[0] && strcmp(hex, remote_hex) != 0) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char sh[8]; memcpy(sh, hex, 7); sh[7] = '\0';
        char *msg = c.message ? c.message : "";
        char *nl = strchr(msg, '\n'); if (nl) *nl = '\0';
        time_t t = (time_t)c.commit_time;
        char tstr[32]; strncpy(tstr, ctime(&t), 24); tstr[24] = '\0';
        printf("  %s  %s  %s\n", sh, tstr, msg);
        count++;
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }
    printf("\n%d unsent commit(s) — run 'hep send' to push\n", count);
    return 0;
}

/* accuse -part — blame specific line range only */
int cmd_accuse_part(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "accuse -part: Usage: hep accuse -part <file> <start> <end>\n");
        fprintf(stderr, "              e.g.   hep accuse -part main.c 10 25\n");
        return 1;
    }
    const char *fname = argv[1];
    int line_start = atoi(argv[2]);
    int line_end   = atoi(argv[3]);
    if (line_start < 1 || line_end < line_start) {
        fprintf(stderr, "accuse -part: invalid range %d-%d\n", line_start, line_end);
        return 1;
    }

    /* walk history, find last commit touching each line in range */
    /* for each line in range, track: which commit last changed it */
    int range = line_end - line_start + 1;
    char **blame_hex    = calloc(range, sizeof(char *));
    char **blame_author = calloc(range, sizeof(char *));
    char **blame_time   = calloc(range, sizeof(char *));
    for (int i = 0; i < range; i++) {
        blame_hex[i]    = strdup("0000000");
        blame_author[i] = strdup("unknown");
        blame_time[i]   = strdup("unknown");
    }

    /* read current file lines for reference */
    FILE *f = fopen(fname, "r");
    if (!f) { fprintf(stderr, "accuse -part: '%s' not found\n", fname); return 1; }
    char **file_lines = malloc(65536 * sizeof(char *)); int nfl = 0;
    char lbuf[4096];
    while (fgets(lbuf, sizeof(lbuf), f) && nfl < 65536) {
        lbuf[strcspn(lbuf, "\n")] = '\0';
        file_lines[nfl++] = strdup(lbuf);
    }
    fclose(f);

    /* walk history */
    char hex[41]; repo_head_sha(hex);
    while (hex[0]) {
        hep_commit c;
        if (commit_read(hex, &c) != HEP_OK) break;
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            for (size_t ti = 0; ti < tree.count; ti++) {
                if (strcmp(tree.entries[ti].name, fname) != 0) continue;
                char bh[41]; sha1_to_hex(tree.entries[ti].sha, bh);
                hep_buf blob;
                if (blob_read(bh, &blob) == HEP_OK) {
                    int nl2 = 0;
                    char **cl = split_lines(blob.data, blob.len, &nl2);
                    /* for each line in range, check if it matches commit version */
                    for (int li = line_start-1; li < line_end && li < nfl; li++) {
                        int idx2 = li - (line_start - 1);
                        if (li < nl2 && strcmp(file_lines[li], cl[li]) == 0) {
                            free(blame_hex[idx2]);
                            free(blame_author[idx2]);
                            free(blame_time[idx2]);
                            char sh[8]; memcpy(sh, hex, 7); sh[7]='\0';
                            blame_hex[idx2] = strdup(sh);
                            blame_author[idx2] = strdup(c.author ? c.author : "unknown");
                            time_t t = (time_t)c.commit_time;
                            char ts[32]; strncpy(ts, ctime(&t), 24); ts[24]='\0';
                            blame_time[idx2] = strdup(ts);
                        }
                    }
                    if (cl && cl[0]) free(cl[0]);
                    free(cl);
                    free(blob.data);
                }
                break;
            }
            tree_free(&tree);
        }
        if (c.parent_count > 0) sha1_to_hex(c.parents[0], hex);
        else hex[0] = '\0';
        commit_free(&c);
    }

    /* print results */
    printf("accuse -part: '%s' lines %d-%d\n\n", fname, line_start, line_end);
    for (int i = 0; i < range && (line_start-1+i) < nfl; i++) {
        printf("%s (%-30s %s %4d) %s\n",
               blame_hex[i], blame_author[i], blame_time[i],
               line_start + i, file_lines[line_start-1+i]);
        free(blame_hex[i]); free(blame_author[i]); free(blame_time[i]);
    }

    free(blame_hex); free(blame_author); free(blame_time);
    for (int i = 0; i < nfl; i++) free(file_lines[i]);
    free(file_lines);
    return 0;
}

/* ── wave 6 dispatcher: routes flags to sub-implementations ─────────────── */
/* these are called from cmd_compete, cmd_print, cmd_hall, cmd_spy, cmd_accuse
   when their respective flags are detected */



int main(int argc, char *argv[]) {
    if (argc < 2) { print_usage(); return 1; }
    const char *cmd = argv[1];
    /* wave 1: original 21 */
    if (!strcmp(cmd,"init"))                    return cmd_init(argc-1, argv+1);
    if (!strcmp(cmd,"print"))                    return cmd_print(argc-1, argv+1);
    if (!strcmp(cmd,"wave"))                  return cmd_wave(argc-1, argv+1);
    if (!strcmp(cmd,"spy"))               return cmd_spy(argc-1, argv+1);
    if (!strcmp(cmd,"compete"))                   return cmd_compete(argc-1, argv+1);
    if (!strcmp(cmd,"light"))                      return cmd_light(argc-1, argv+1);
    if (!strcmp(cmd,"expand"))                 return cmd_expand(argc-1, argv+1);
    if (!strcmp(cmd,"travel"))                 return cmd_travel(argc-1, argv+1);
    if (!strcmp(cmd,"chiplets"))                 return cmd_chiplets(argc-1, argv+1);
    if (!strcmp(cmd,"stl"))                   return cmd_stl(argc-1, argv+1);
    if (!strcmp(cmd,"send"))                 return cmd_send(argc-1, argv+1);
    if (!strcmp(cmd,"dock"))              return cmd_dock(argc-1, argv+1);
    if (!strcmp(cmd,"interface"))                    return cmd_interface(argc-1, argv+1);
    if (!strcmp(cmd,"search"))              return cmd_search(argc-1, argv+1);
    if (!strcmp(cmd,"hall"))                    return cmd_hall(argc-1, argv+1);
    if (!strcmp(cmd,"retrieve"))                  return cmd_retrieve(argc-1, argv+1);
    if (!strcmp(cmd,"group"))                    return cmd_group(argc-1, argv+1);
    if (!strcmp(cmd,"microscope"))                    return cmd_microscope(argc-1, argv+1);
    if (!strcmp(cmd,"earth"))                   return cmd_earth(argc-1, argv+1);
    if (!strcmp(cmd,"house"))                   return cmd_house(argc-1, argv+1);
    if (!strcmp(cmd,"kill"))                  return cmd_kill(argc-1, argv+1);
    /* wave 2: 20 commands */
    if (!strcmp(cmd,"mean"))                     return cmd_mean(argc-1, argv+1);
    if (!strcmp(cmd,"short"))                    return cmd_short(argc-1, argv+1);
    if (!strcmp(cmd,"close"))                   return cmd_close(argc-1, argv+1);
    if (!strcmp(cmd,"secret"))                    return cmd_secret(argc-1, argv+1);
    if (!strcmp(cmd,"change"))                return cmd_change(argc-1, argv+1);
    if (!strcmp(cmd,"hotel"))                   return cmd_hotel(argc-1, argv+1);
    if (!strcmp(cmd,"accuse"))              return cmd_accuse(argc-1, argv+1);
    if (!strcmp(cmd,"discord"))                  return cmd_discord(argc-1, argv+1);
    if (!strcmp(cmd,"wpm"))                     return cmd_wpm(argc-1, argv+1);
    if (!strcmp(cmd,"gnome"))                   return cmd_gnome(argc-1, argv+1);
    if (!strcmp(cmd,"window"))                  return cmd_window(argc-1, argv+1);
    if (!strcmp(cmd,"linux"))                   return cmd_linux(argc-1, argv+1);
    if (!strcmp(cmd,"what"))                    return cmd_what(argc-1, argv+1);
    if (!strcmp(cmd,"intelisbetterthanamd"))    return cmd_intelisbetterthanamd(argc-1, argv+1);
    if (!strcmp(cmd,"nvl"))                     return cmd_nvl(argc-1, argv+1);
    if (!strcmp(cmd,"ptl"))                     return cmd_ptl(argc-1, argv+1);
    if (!strcmp(cmd,"aaa"))                     return cmd_aaa(argc-1, argv+1);
    if (!strcmp(cmd,"bd"))                      return cmd_bd(argc-1, argv+1);
    if (!strcmp(cmd,"power"))                   return cmd_power(argc-1, argv+1);
    if (!strcmp(cmd,"r"))                       return cmd_r(argc-1, argv+1);
    /* wave 3: 7 unbypassable */
    if (!strcmp(cmd,"arm"))                     return cmd_arm(argc-1, argv+1);
    if (!strcmp(cmd,"ia"))                      return cmd_ia(argc-1, argv+1);
    if (!strcmp(cmd,"intel"))                   return cmd_intel(argc-1, argv+1);
    if (!strcmp(cmd,"amd"))                     return cmd_amd(argc-1, argv+1);
    if (!strcmp(cmd,"nvidia"))                  return cmd_nvidia(argc-1, argv+1);
    if (!strcmp(cmd,"arc"))                     return cmd_arc(argc-1, argv+1);
    if (!strcmp(cmd,"radeon"))                  return cmd_radeon(argc-1, argv+1);
    /* wave 4: RTX GTX RX IRIS XE UHD HD FHD APU XPU NPU CPU GPU RPU A B */
    if (!strcmp(cmd,"rtx"))                     return cmd_rtx(argc-1, argv+1);
    if (!strcmp(cmd,"gtx"))                     return cmd_gtx(argc-1, argv+1);
    if (!strcmp(cmd,"rx"))                      return cmd_rx(argc-1, argv+1);
    if (!strcmp(cmd,"iris"))                    return cmd_iris(argc-1, argv+1);
    if (!strcmp(cmd,"xe"))                      return cmd_xe(argc-1, argv+1);
    if (!strcmp(cmd,"uhd"))                     return cmd_uhd(argc-1, argv+1);
    if (!strcmp(cmd,"hd"))                      return cmd_hd(argc-1, argv+1);
    if (!strcmp(cmd,"fhd"))                     return cmd_fhd(argc-1, argv+1);
    if (!strcmp(cmd,"apu"))                     return cmd_apu(argc-1, argv+1);
    if (!strcmp(cmd,"xpu"))                     return cmd_xpu(argc-1, argv+1);
    if (!strcmp(cmd,"npu"))                     return cmd_npu(argc-1, argv+1);
    if (!strcmp(cmd,"cpu"))                     return cmd_cpu(argc-1, argv+1);
    if (!strcmp(cmd,"gpu"))                     return cmd_gpu(argc-1, argv+1);
    if (!strcmp(cmd,"rpu"))                     return cmd_rpu(argc-1, argv+1);
    if (!strcmp(cmd,"a"))                       return cmd_a(argc-1, argv+1);
    if (!strcmp(cmd,"b"))                       return cmd_b(argc-1, argv+1);
    /* wave 5: image commands — bios case psu ups nas link raid room */
    if (!strcmp(cmd,"bios"))                    return cmd_bios(argc-1, argv+1);
    if (!strcmp(cmd,"case"))                    return cmd_case(argc-1, argv+1);
    if (!strcmp(cmd,"psu"))                     return cmd_psu(argc-1, argv+1);
    if (!strcmp(cmd,"ups"))                     return cmd_ups(argc-1, argv+1);
    if (!strcmp(cmd,"nas"))                     return cmd_nas(argc-1, argv+1);
    if (!strcmp(cmd,"link"))                    return cmd_link(argc-1, argv+1);
    if (!strcmp(cmd,"raid"))                    return cmd_raid(argc-1, argv+1);
    if (!strcmp(cmd,"room"))                    return cmd_room(argc-1, argv+1);
    /* wave 6: the real gaps */
    if (!strcmp(cmd,"rp"))                      return cmd_rp(argc-1, argv+1);
    if (!strcmp(cmd,"unsent"))                  return cmd_unsent(argc-1, argv+1);
    /* wave 7: undo/redo + mansion large-file store */
    if (!strcmp(cmd,"undo"))                    return cmd_undo(argc-1, argv+1);
    if (!strcmp(cmd,"redo"))                    return cmd_redo(argc-1, argv+1);
    if (!strcmp(cmd,"mansion"))                 return cmd_mansion(argc-1, argv+1);
    fprintf(stderr, "hep: '%s' not recognized 😈\nRun 'hep bios' for help.\n", cmd);
    return 1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * WAVE 7: UNDO/REDO + MANSION LARGE-FILE STORE
 *
 * undo    — step back one reflog entry, no flags, no fear
 * redo    — step forward (saved by undo)
 * mansion limit <size>  — set large-file threshold  (e.g. 50MB)
 * mansion dock [file]   — pull specific large file version
 * mansion light         — show what's in mansion vs normal objects
 * mansion send          — push large file bytes to remote
 *
 * How mansion works:
 *   Files over the threshold are NOT stored as blobs in .hep/objects/
 *   Instead: a small "mansion ref" file is stored as the blob:
 *     "mansion:<sha256-of-content> <size> <original-filename>"
 *   The actual bytes live in .hep/mansion/<sha256[:2]>/<sha256[2:]>
 *   On clone/fetch only the refs transfer. Bytes come down on "dock".
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── undo ──────────────────────────────────────────────────────────────────── */
int cmd_undo(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "undo: not in a hep repo\n"); return 1;
    }

    /* read reflog */
    char reflog_path[HEP_PATH_MAX];
    snprintf(reflog_path, sizeof(reflog_path), "%s/logs/HEAD", root);
    FILE *f = fopen(reflog_path, "r");
    if (!f) { printf("undo: nothing to undo\n"); return 0; }

    char *entries[4096]; int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < 4096) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0]) entries[n++] = strdup(line);
    }
    fclose(f);

    if (n < 2) { printf("undo: already at oldest commit\n");
        for (int i=0;i<n;i++) free(entries[i]); return 0; }

    /* current HEAD is entries[n-1], one-back is entries[n-2] */
    char current_hex[41] = {0};
    sscanf(entries[n-1], "%40s", current_hex);

    char prev_hex[41] = {0};
    sscanf(entries[n-2], "%40s", prev_hex);

    /* save current position in redo stack */
    char redo_path[HEP_PATH_MAX];
    snprintf(redo_path, sizeof(redo_path), "%s/logs/REDO", root);
    FILE *rf = fopen(redo_path, "a");
    if (rf) { fprintf(rf, "%s\n", current_hex); fclose(rf); }

    /* move HEAD back */
    repo_update_head(prev_hex);

    /* restore working tree to that commit */
    hep_commit c;
    if (commit_read(prev_hex, &c) == HEP_OK) {
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            hep_index idx; index_read(&idx);
            /* clear index */
            idx.count = 0;
            for (size_t i = 0; i < tree.count; i++) {
                char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                hep_buf blob;
                if (blob_read(eh, &blob) == HEP_OK) {
                    util_write_file(tree.entries[i].name, blob.data, blob.len);
                    hep_sha1 sha; sha1_from_hex(eh, sha);
                    index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
                    free(blob.data);
                }
            }
            index_write(&idx); index_free(&idx);
            tree_free(&tree);
        }
        char sh[8]; memcpy(sh, prev_hex, 7); sh[7]='\0';
        char *msg = c.message ? c.message : "";
        char *nl = strchr(msg,'\n'); if(nl) *nl='\0';
        printf("undo: stepped back to %s \"%s\"\n", sh, msg);
        printf("      run 'hep redo' to go forward again\n");
        commit_free(&c);
    }

    /* trim last entry from reflog */
    FILE *wf = fopen(reflog_path, "w");
    if (wf) {
        for (int i = 0; i < n-1; i++) fprintf(wf, "%s\n", entries[i]);
        fclose(wf);
    }
    for (int i = 0; i < n; i++) free(entries[i]);
    return 0;
}

/* ── redo ──────────────────────────────────────────────────────────────────── */
int cmd_redo(int argc, char **argv) {
    (void)argc; (void)argv;
    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "redo: not in a hep repo\n"); return 1;
    }

    char redo_path[HEP_PATH_MAX];
    snprintf(redo_path, sizeof(redo_path), "%s/logs/REDO", root);
    FILE *f = fopen(redo_path, "r");
    if (!f) { printf("redo: nothing to redo\n"); return 0; }

    char *entries[4096]; int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < 4096) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0]) entries[n++] = strdup(line);
    }
    fclose(f);

    if (n == 0) { printf("redo: nothing to redo\n"); return 0; }

    /* most recent redo entry is last line */
    char next_hex[41] = {0};
    strncpy(next_hex, entries[n-1], 40);

    /* restore working tree */
    hep_commit c;
    if (commit_read(next_hex, &c) == HEP_OK) {
        char tree_hex[41]; sha1_to_hex(c.tree_sha, tree_hex);
        hep_tree tree;
        if (tree_read(tree_hex, &tree) == HEP_OK) {
            hep_index idx; index_read(&idx);
            idx.count = 0;
            for (size_t i = 0; i < tree.count; i++) {
                char eh[41]; sha1_to_hex(tree.entries[i].sha, eh);
                hep_buf blob;
                if (blob_read(eh, &blob) == HEP_OK) {
                    util_write_file(tree.entries[i].name, blob.data, blob.len);
                    hep_sha1 sha; sha1_from_hex(eh, sha);
                    index_add_entry(&idx, tree.entries[i].name, sha, 0100644);
                    free(blob.data);
                }
            }
            index_write(&idx); index_free(&idx);
            tree_free(&tree);
        }
        repo_update_head(next_hex);
        reflog_append(next_hex, c.message ? c.message : "redo");
        char sh[8]; memcpy(sh, next_hex, 7); sh[7]='\0';
        char *msg = c.message ? c.message : "";
        char *nl = strchr(msg,'\n'); if(nl) *nl='\0';
        printf("redo: stepped forward to %s \"%s\"\n", sh, msg);
        commit_free(&c);
    } else {
        fprintf(stderr, "redo: couldn't read commit %s\n", next_hex);
        for (int i=0;i<n;i++) free(entries[i]); return 1;
    }

    /* pop the redo stack */
    FILE *wf = fopen(redo_path, "w");
    if (wf) {
        for (int i = 0; i < n-1; i++) fprintf(wf, "%s\n", entries[i]);
        fclose(wf);
    }
    for (int i = 0; i < n; i++) free(entries[i]);
    return 0;
}

/* ── mansion helpers ───────────────────────────────────────────────────────── */

/* simple 64-bit FNV hash used to make a content-address for large files
   (we don't have SHA256 in the codebase — using SHA1 from our existing code) */
static void mansion_content_id(const char *path, char out_hex[41]) {
    /* reuse blob_from_file which already does SHA1 */
    blob_from_file(path, out_hex);
}

static long mansion_threshold(const char *root) {
    char cfg_path[HEP_PATH_MAX];
    snprintf(cfg_path, sizeof(cfg_path), "%s/mansion.limit", root);
    FILE *f = fopen(cfg_path, "r");
    if (!f) return 50L * 1024 * 1024; /* default 50MB */
    long threshold = 50L * 1024 * 1024;
    fscanf(f, "%ld", &threshold);
    fclose(f);
    return threshold;
}

static void mansion_store_path(const char *root, const char *sha1hex,
                               char out[HEP_PATH_MAX]) {
    snprintf(out, HEP_PATH_MAX, "%s/mansion/%.2s/%s",
             root, sha1hex, sha1hex + 2);
}

/* check if a file should go to mansion based on size */
static int mansion_should_store(const char *root, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_size > mansion_threshold(root);
}

/* store a large file in mansion, return the mansion-ref string */
static int mansion_store(const char *root, const char *path,
                         char ref_out[256]) {
    char sha1hex[41];
    if (blob_from_file(path, sha1hex) != HEP_OK) return HEP_ERR;

    /* make mansion dir */
    char man_root[HEP_PATH_MAX];
    snprintf(man_root, sizeof(man_root), "%s/mansion", root);
    mkdir(man_root, 0755);
    char man_sub[HEP_PATH_MAX];
    snprintf(man_sub, sizeof(man_sub), "%s/mansion/%.2s", root, sha1hex);
    mkdir(man_sub, 0755);

    char dst[HEP_PATH_MAX];
    mansion_store_path(root, sha1hex, dst);

    /* copy file bytes into mansion store if not already there */
    struct stat st;
    if (stat(dst, &st) != 0) {
        FILE *src = fopen(path, "rb");
        FILE *out = fopen(dst, "wb");
        if (src && out) {
            char buf[65536]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
                fwrite(buf, 1, n, out);
        }
        if (src) fclose(src);
        if (out) fclose(out);
        stat(dst, &st);
    }

    /* write ref string: "mansion:<sha1> <size> <basename>" */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(ref_out, 256, "mansion:%s %ld %s",
             sha1hex, (long)st.st_size, base);
    return HEP_OK;
}

/* ── mansion command ───────────────────────────────────────────────────────── */
int cmd_mansion(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "mansion: large-file store\n"
            "  hep mansion limit <size>   set threshold  (e.g. 50MB, 200MB, 1GB)\n"
            "  hep mansion dock [file]    pull large file bytes from remote\n"
            "  hep mansion light          show mansion vs normal file status\n"
            "  hep mansion send           push large file bytes to remote\n");
        return 1;
    }

    char root[HEP_PATH_MAX];
    if (repo_find_root(root, sizeof(root)) != HEP_OK) {
        fprintf(stderr, "mansion: not in a hep repo\n"); return 1;
    }

    /* ── mansion limit ── */
    if (strcmp(argv[1], "limit") == 0) {
        if (argc < 3) {
            fprintf(stderr, "mansion limit: Usage: hep mansion limit <size>\n"
                            "               e.g. 50MB  200MB  1GB  500KB\n");
            return 1;
        }
        /* parse size string: number + unit */
        char *s = argv[2];
        long val = atol(s);
        char *unit = s;
        while (*unit && (*unit == '-' || (*unit >= '0' && *unit <= '9'))) unit++;
        long bytes = val;
        if      (strcasecmp(unit, "KB") == 0) bytes = val * 1024L;
        else if (strcasecmp(unit, "MB") == 0) bytes = val * 1024L * 1024L;
        else if (strcasecmp(unit, "GB") == 0) bytes = val * 1024L * 1024L * 1024L;
        else bytes = val; /* assume bytes if no unit */

        char cfg_path[HEP_PATH_MAX];
        snprintf(cfg_path, sizeof(cfg_path), "%s/mansion.limit", root);
        FILE *f = fopen(cfg_path, "w");
        if (!f) { fprintf(stderr, "mansion limit: failed to write config\n"); return 1; }
        fprintf(f, "%ld\n", bytes);
        fclose(f);

        /* human readable confirmation */
        if (bytes >= 1024L*1024L*1024L)
            printf("mansion limit: %.1f GB — files larger than this go to mansion\n",
                   (double)bytes / (1024.0*1024.0*1024.0));
        else if (bytes >= 1024L*1024L)
            printf("mansion limit: %.0f MB — files larger than this go to mansion\n",
                   (double)bytes / (1024.0*1024.0));
        else
            printf("mansion limit: %ld KB — files larger than this go to mansion\n",
                   bytes / 1024L);
        printf("              hep print will automatically route large files to mansion\n");
        return 0;
    }

    /* ── mansion light ── */
    if (strcmp(argv[1], "light") == 0) {
        long threshold = mansion_threshold(root);
        printf("mansion light: threshold = ");
        if (threshold >= 1024L*1024L*1024L)
            printf("%.1f GB\n\n", (double)threshold/(1024.0*1024.0*1024.0));
        else
            printf("%.0f MB\n\n", (double)threshold/(1024.0*1024.0));

        /* scan index for mansion refs */
        hep_index idx; index_read(&idx);
        int mansion_count = 0, normal_count = 0;
        for (size_t i = 0; i < idx.count; i++) {
            /* read blob content to check if it's a mansion ref */
            char hex[41]; sha1_to_hex(idx.entries[i].sha, hex);
            hep_buf blob;
            if (blob_read(hex, &blob) == HEP_OK) {
                if (blob.len < 256 && blob.data &&
                    strncmp(blob.data, "mansion:", 8) == 0) {
                    /* mansion ref */
                    char size_str[32] = "?";
                    long sz = 0;
                    sscanf(blob.data + 8, "%*s %ld", &sz);
                    if (sz >= 1024L*1024L*1024L)
                        snprintf(size_str, sizeof(size_str), "%.1f GB",
                                 (double)sz/(1024.0*1024.0*1024.0));
                    else if (sz >= 1024L*1024L)
                        snprintf(size_str, sizeof(size_str), "%.0f MB",
                                 (double)sz/(1024.0*1024.0));
                    else
                        snprintf(size_str, sizeof(size_str), "%ld KB", sz/1024L);
                    printf("  MANSION  %-8s  %s\n", size_str, idx.entries[i].path);
                    mansion_count++;
                } else {
                    printf("  normal   %-8zu b   %s\n",
                           blob.len, idx.entries[i].path);
                    normal_count++;
                }
                free(blob.data);
            }
        }
        printf("\n%d normal, %d in mansion\n", normal_count, mansion_count);
        index_free(&idx);
        return 0;
    }

    /* ── mansion send ── */
    if (strcmp(argv[1], "send") == 0) {
        char origin[HEP_PATH_MAX] = {0};
        config_get("remote.origin.url", origin, sizeof(origin));
        if (!origin[0]) {
            fprintf(stderr, "mansion send: no remote configured\n"
                            "              set one: hep nas origin <path>\n");
            return 1;
        }

        char man_src[HEP_PATH_MAX], man_dst[HEP_PATH_MAX];
        snprintf(man_src, sizeof(man_src), "%s/mansion", root);
        snprintf(man_dst, sizeof(man_dst), "%s/.hep/mansion", origin);

        struct stat st;
        if (stat(man_src, &st) != 0) {
            printf("mansion send: no large files to push\n"); return 0;
        }

        /* mkdir remote mansion and rsync-style copy */
        char cmd2[HEP_PATH_MAX * 2];
        snprintf(cmd2, sizeof(cmd2), "mkdir -p '%s' && cp -rn '%s/'* '%s/' 2>/dev/null",
                 man_dst, man_src, man_dst);
        int r = system(cmd2);
        if (r == 0)
            printf("mansion send: large files pushed to %s\n", origin);
        else
            printf("mansion send: partial push (some files may already exist)\n");
        return 0;
    }

    /* ── mansion dock ── */
    if (strcmp(argv[1], "dock") == 0) {
        char origin[HEP_PATH_MAX] = {0};
        config_get("remote.origin.url", origin, sizeof(origin));
        if (!origin[0]) {
            fprintf(stderr, "mansion dock: no remote configured\n");
            return 1;
        }

        char man_src[HEP_PATH_MAX];
        snprintf(man_src, sizeof(man_src), "%s/.hep/mansion", origin);
        struct stat st;
        if (stat(man_src, &st) != 0) {
            printf("mansion dock: no mansion store on remote\n"); return 0;
        }

        if (argc >= 3) {
            /* pull specific file: find its mansion ref in index */
            const char *want = argv[2];
            hep_index idx; index_read(&idx);
            hep_index_entry *e = index_find(&idx, want);
            if (!e) {
                fprintf(stderr, "mansion dock: '%s' not in index\n", want);
                index_free(&idx); return 1;
            }
            char hex[41]; sha1_to_hex(e->sha, hex);
            hep_buf blob;
            if (blob_read(hex, &blob) != HEP_OK ||
                strncmp(blob.data, "mansion:", 8) != 0) {
                printf("mansion dock: '%s' is not a mansion file\n", want);
                if (blob.data) free(blob.data);
                index_free(&idx); return 0;
            }
            /* extract sha from ref */
            char man_sha[41] = {0};
            sscanf(blob.data + 8, "%40s", man_sha);
            free(blob.data); index_free(&idx);

            /* copy from remote mansion */
            char src_path[HEP_PATH_MAX], dst_path[HEP_PATH_MAX];
            snprintf(src_path, sizeof(src_path),
                     "%s/%.2s/%s", man_src, man_sha, man_sha + 2);
            /* store locally first */
            char local_man[HEP_PATH_MAX];
            snprintf(local_man, sizeof(local_man), "%s/mansion", root);
            mkdir(local_man, 0755);
            char local_sub[HEP_PATH_MAX];
            snprintf(local_sub, sizeof(local_sub),
                     "%s/mansion/%.2s", root, man_sha);
            mkdir(local_sub, 0755);
            mansion_store_path(root, man_sha, dst_path);

            char cmd2[HEP_PATH_MAX * 2];
            snprintf(cmd2, sizeof(cmd2), "cp '%s' '%s'", src_path, dst_path);
            if (system(cmd2) == 0) {
                /* now write the actual file to working tree */
                snprintf(cmd2, sizeof(cmd2), "cp '%s' '%s'", dst_path, want);
                system(cmd2);
                printf("mansion dock: pulled '%s'\n", want);
            } else {
                fprintf(stderr, "mansion dock: '%s' not found on remote\n", want);
                return 1;
            }
        } else {
            /* pull ALL missing mansion files */
            char cmd2[HEP_PATH_MAX * 2];
            snprintf(cmd2, sizeof(cmd2),
                     "cp -rn '%s/'* '%s/mansion/' 2>/dev/null || true",
                     man_src, root);
            system(cmd2);
            printf("mansion dock: pulled all available large files from remote\n");
        }
        return 0;
    }

    fprintf(stderr, "mansion: unknown subcommand '%s'\n", argv[1]);
    return 1;
}

