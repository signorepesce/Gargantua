#ifndef GARGANTUA_FETCH_INTERNAL_H
#define GARGANTUA_FETCH_INTERNAL_H

int channel_open(Channel *channel, const Target *target, const struct timespec *deadline);

ssize_t channel_read(Channel *channel, char *out, size_t cap, const struct timespec *deadline);

ssize_t channel_write(Channel *channel, const char *data, size_t len, const struct timespec *deadline);

int connect_target(const Target *target, const struct timespec *deadline);

Response failure(int code, str message);

int header_value_safe(const char *value);

int host_allowed(const char *host);

int make_deadline(int timeout_ms, struct timespec *deadline);

int parse_reply(char *wire, size_t len, int *status_out, char **body_out, size_t *body_len);

int parse_url(const char *url, Target *target);

int resolve_request(str url, Target *target);

extern _Thread_local char g_wire[FETCH_MAX_RESPONSE];

int build_request(char *out, size_t cap, const char *method, const Target *target, const char *content_type, size_t body_len);

void channel_close(Channel *channel);

int read_reply(Channel *channel, size_t *len, const struct timespec *deadline);

int fetch_send_all(Channel *channel, const char *data, size_t len, const struct timespec *deadline);

int tls_open(Channel *channel, const Target *target, const struct timespec *deadline);

#ifdef GARGANTUA_TLS
extern SSL_CTX *g_tls_context;
#endif

#endif
