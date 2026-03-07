#ifndef HEP_TYPES_H
#define HEP_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define HEP_SHA1_LEN     20
#define HEP_SHA1_HEX_LEN 40
#define HEP_PATH_MAX     4096
#define HEP_MAX_PARENTS  32

/* object types */
typedef enum {
    OBJ_BLOB   = 1,
    OBJ_TREE   = 2,
    OBJ_COMMIT = 3,
    OBJ_TAG    = 4
} hep_obj_type;

/* raw bytes + length */
typedef struct {
    uint8_t *data;
    size_t   len;
} hep_buf;

/* 20-byte SHA1 hash */
typedef uint8_t hep_sha1[HEP_SHA1_LEN];

/* a single tree entry */
typedef struct {
    char     name[256];
    hep_sha1 sha;
    uint32_t mode;  /* 0100644 file, 0100755 exec, 040000 dir */
} hep_tree_entry;

/* parsed tree object */
typedef struct {
    hep_tree_entry *entries;
    size_t          count;
} hep_tree;

/* parsed commit object */
typedef struct {
    hep_sha1  tree_sha;
    hep_sha1  parents[HEP_MAX_PARENTS];
    int       parent_count;
    char     *author;
    char     *committer;
    int64_t   author_time;
    int64_t   commit_time;
    char     *message;
} hep_commit;

/* index entry (one staged file) */
typedef struct {
    char     path[HEP_PATH_MAX];
    hep_sha1 sha;
    uint32_t mode;
    uint32_t mtime;
    uint32_t size;
} hep_index_entry;

/* the whole index */
typedef struct {
    hep_index_entry *entries;
    size_t           count;
    size_t           cap;
} hep_index;

/* error codes */
#define HEP_OK           0
#define HEP_ERR         -1
#define HEP_ERR_NOENT   -2
#define HEP_ERR_EXISTS  -3
#define HEP_ERR_IO      -4
#define HEP_ERR_CORRUPT -5
#define HEP_ERR_NOREPO  -6

#endif /* HEP_TYPES_H */
