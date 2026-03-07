#ifndef HEP_COMMIT_H
#define HEP_COMMIT_H
#include "types.h"
int  commit_write(hep_commit *c, char out_hex[41]);
int  commit_read(const char *hex, hep_commit *out);
void commit_free(hep_commit *c);
#endif
