#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_MAX_KEYS 64
#define CONFIG_KEY_LEN  64
#define CONFIG_VALUE_LEN  256
#ifndef CONFIG_FILE
#define CONFIG_FILE     "app/application.properties"
#endif

int config_load(const char *path);

const char *config_str(const char *key, const char *fallback);

int config_int(const char *key, int fallback);

#endif
