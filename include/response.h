#ifndef RESPONSE_H
#define RESPONSE_H

#include <stddef.h>

#define RESPONSE_MAX_HEADERS 16
#define RESPONSE_NAME_CAP 64
#define RESPONSE_VALUE_CAP 1024
#define RESPONSE_WIRE_CAP (RESPONSE_MAX_HEADERS * (RESPONSE_NAME_CAP + RESPONSE_VALUE_CAP + 4))

int response_header(const char *name, const char *value);
int response_location(const char *value);
int response_status(int code);
void response_reset(void);
int response_status_get(int fallback);
int response_write(char *out, size_t cap);
int response_token(const char *text);
int response_value(const char *text);

#define $response_header(name, value) response_header((name), (value))
#define $location(value) response_location((value))
#define $status(code) response_status((code))

#endif
