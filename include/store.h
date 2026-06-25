#ifndef STORE_H
#define STORE_H

#include "gargantua.h"

#include <pthread.h>
#include <stddef.h>

#define STORE_TEXT_LEN 256
#define STORE_MAX_ROWS 256
#define STORE_MAX_FIELDS 64

typedef struct
{
    const TypeInfo *type;
    void           *rows;
    char           *text;
    int             capacity;
    int             count;
    int             text_fields;
    pthread_mutex_t lock;
} Store;

int     store_count(Store *store);
int     store_add(Store *store, const void *value);
int     store_put(Store *store, int index, const void *value);
int     store_get(Store *store, int index, void *out);
RowList store_list(Store *store);
void    store_clear(Store *store);
int     store_text_fields(const TypeInfo *type);

#endif
