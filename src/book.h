#ifndef NEST_BOOK_H
#define NEST_BOOK_H
#include <stddef.h>
int book_list_images(const char *path, char ***out); /* naturally sorted image entries */
void book_free_list(char **names, int n);
void *book_read_entry(const char *path, const char *name, size_t *size);
#endif
