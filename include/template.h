#ifndef TEMPLATE_H
#define TEMPLATE_H

#include "gargantua.h"

#define TEMPLATE_MAX_PATH  1024
#define TEMPLATE_MAX_BYTES (128 * 1024)
#define TEMPLATE_MAX_OUT   (256 * 1024)
#define TEMPLATE_MAX_VALUE 1024

int template_init(void);
int template_enabled(void);

str render_template(str name, RowList rows);

#define $render(name, rows) render_template((name), (rows))

#endif
