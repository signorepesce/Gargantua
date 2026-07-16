#ifndef GARGANTUA_SERVER_INTERNAL_H
#define GARGANTUA_SERVER_INTERNAL_H

int client_limit(uint32_t ip, int action);

int consume_message(Worker *w, const HttpRequest *req, size_t *len);

void cors_prepare(const HttpRequest *req);

int deadline_after(struct timespec *deadline, int seconds);

void finish_request(Worker *w);

int is_health_path(const char *path);

int keep_alive_wanted(const HttpRequest *req, int served);

void log_request(const struct timeval *start, const char *method, const char *path, int status);

int queue_pop(uint32_t *ip);

int read_request(int sock, char *buf, size_t cap, size_t *len, HttpRequest *req);

void reset_response_state(void);

int send_all(int sock, const char *buf, size_t len);

int send_bytes(int sock, int status, const char *content_type, const char *body, size_t blen, int keep_alive);

void send_read_error(int sock, int rc);

int send_response(int sock, int status, const char *content_type, const char *body, int keep_alive);

int serve_health(int sock, const HttpRequest *req, const char *path, int keep_alive, int *status_out);

int serve_preflight(int sock, const HttpRequest *req, int keep_alive, int *status_out, int *handled);

int serve_request(Worker *w, int sock, const HttpRequest *req, char *path, char *query, char *body_in, size_t body_len, int keep_alive, int *status_out);

int set_timeout_until(int sock, int option, const struct timespec *deadline);

int split_path(char *path, char **query);

extern _Atomic int g_drain_over;

extern ConnQueue g_queue;

extern _Thread_local char response_allow[ROUTE_ALLOW_CAP];

extern _Thread_local int response_cors;

extern _Thread_local int response_head;

extern _Thread_local char response_origin[RESPONSE_VALUE_CAP];

extern _Thread_local int response_preflight;

extern _Thread_local char response_requested_headers[RESPONSE_VALUE_CAP];

extern _Thread_local const char *response_route;

int queue_push(int fd, uint32_t ip);

int request_header_has_token(const HttpRequest *req, const char *name, const char *wanted);

extern int g_connections_per_ip;

extern int          g_drain_ms;

extern int g_requests_per_minute;

extern _Atomic int g_stop;

extern pthread_t g_worker_threads[SERVER_WORKERS];

extern Worker    g_workers[SERVER_WORKERS];

extern int       g_workers_started;

extern ClientLimit g_clients[256];

void on_signal(int sig);

#endif
