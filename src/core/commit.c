/* src/core/commit.c */
#include "commit.h"
#include "odb.h"
#include "sha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Commit text format:
 *   tree <hex>\n
 *   parent <hex>\n          (0 or more)
 *   author <name> <time>\n
 *   committer <name> <time>\n
 *   \n
 *   <message>\n
 */

int commit_write(hep_commit *c, char out_hex[41]) {
    char tree_hex[41];
    sha1_to_hex(c->tree_sha, tree_hex);

    /* build text */
    size_t cap = 4096 + (c->message ? strlen(c->message) : 0);
    char *buf = malloc(cap);
    if (!buf) return HEP_ERR;

    int pos = 0;
    pos += snprintf(buf+pos, cap-(size_t)pos, "tree %s\n", tree_hex);
    for (int i = 0; i < c->parent_count; i++) {
        char ph[41]; sha1_to_hex(c->parents[i], ph);
        pos += snprintf(buf+pos, cap-(size_t)pos, "parent %s\n", ph);
    }
    pos += snprintf(buf+pos, cap-(size_t)pos, "author %s %lld +0000\n",
                    c->author    ? c->author    : "unknown",
                    (long long)c->author_time);
    pos += snprintf(buf+pos, cap-(size_t)pos, "committer %s %lld +0000\n",
                    c->committer ? c->committer : "unknown",
                    (long long)c->commit_time);
    pos += snprintf(buf+pos, cap-(size_t)pos, "\n%s\n",
                    c->message   ? c->message   : "");

    int r = odb_write(OBJ_COMMIT, (uint8_t*)buf, (size_t)pos, out_hex);
    free(buf);
    return r;
}

int commit_read(const char *hex, hep_commit *out) {
    hep_obj_type t;
    hep_buf buf;
    int r = odb_read(hex, &t, &buf);
    if (r != HEP_OK) return r;
    if (t != OBJ_COMMIT) { free(buf.data); return HEP_ERR_CORRUPT; }

    memset(out, 0, sizeof(*out));
    char *p = (char*)buf.data;
    char *end = p + buf.len;

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        *nl = '\0';

        if (strncmp(p, "tree ", 5) == 0) {
            sha1_from_hex(p+5, out->tree_sha);
        } else if (strncmp(p, "parent ", 7) == 0 &&
                   out->parent_count < HEP_MAX_PARENTS) {
            sha1_from_hex(p+7, out->parents[out->parent_count++]);
        } else if (strncmp(p, "author ", 7) == 0) {
            out->author = strdup(p+7);
            /* parse time: last token before " +XXXX" */
            char *ts = strrchr(out->author, ' ');
            if (ts) { *ts = '\0'; }
            ts = strrchr(out->author, ' ');
            if (ts) { out->author_time = atoll(ts+1); *ts = '\0'; }
        } else if (strncmp(p, "committer ", 10) == 0) {
            out->committer = strdup(p+10);
            char *ts = strrchr(out->committer, ' ');
            if (ts) { *ts = '\0'; }
            ts = strrchr(out->committer, ' ');
            if (ts) { out->commit_time = atoll(ts+1); *ts = '\0'; }
        } else if (*p == '\0') {
            /* blank line — rest is message */
            p = nl + 1;
            out->message = strdup(p);
            break;
        }
        p = nl + 1;
    }

    free(buf.data);
    return HEP_OK;
}

void commit_free(hep_commit *c) {
    free(c->author);
    free(c->committer);
    free(c->message);
    memset(c, 0, sizeof(*c));
}
