#ifndef HEP_TREE_H
#define HEP_TREE_H
#include "types.h"
int  tree_write(hep_tree *tree, char out_hex[41]);
int  tree_read(const char *hex, hep_tree *out);
void tree_free(hep_tree *tree);
#endif
