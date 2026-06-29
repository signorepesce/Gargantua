#ifndef GARGANTUA_RUNTIME_INTERNAL_H
#define GARGANTUA_RUNTIME_INTERNAL_H

void body_reset(void);

int field_validate(const FieldInfo *f, const void *obj, const char *path);

int is_safe_identifier(const char *name);

bool read_bool(const void *obj, unsigned short offset);

double read_double(const void *obj, unsigned short offset);

int read_int(const void *obj, unsigned short offset);

long read_long(const void *obj, unsigned short offset);

const char *read_str(const void *obj, unsigned short offset);

void sink_add(Sink *s, const char *text);

void sink_add_escaped(Sink *s, const char *text);

void sink_init(Sink *s, char *buf, size_t cap);

void sink_put(Sink *s, char c);

int type_valid(const TypeInfo *type);

#endif
