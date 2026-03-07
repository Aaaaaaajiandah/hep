#ifndef HEP_INDEX_H
#define HEP_INDEX_H
#include "core/types.h"
int               index_read(hep_index *idx);
int               index_write(hep_index *idx);
void              index_free(hep_index *idx);
int               index_add_entry(hep_index *idx, const char *path,
                                  const uint8_t sha[20], uint32_t mode);
int               index_remove_entry(hep_index *idx, const char *path);
hep_index_entry  *index_find(hep_index *idx, const char *path);
#endif
