#ifndef NEST_CBR_H
#define NEST_CBR_H
#include <stddef.h>
int rar_list(const char *path, char ***names_out); /* image entries in archive order */
void *rar_read(const char *path, const char *name, size_t *size);
#endif
