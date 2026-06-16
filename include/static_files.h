#ifndef STATIC_FILES_H
#define STATIC_FILES_H

#include <stddef.h>

#define STATIC_MAX_PATH  1024
#define STATIC_MAX_BYTES (256 * 1024)

int static_files_init(void);
int static_files_enabled(void);
int static_file_read(const char *url_path, char *out, size_t cap, size_t *len, const char **content_type);

#endif
