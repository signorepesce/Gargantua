#ifndef SERVER_H
#define SERVER_H

#define SERVER_WORKERS    8
#define SERVER_QUEUE      64
#define SERVER_STACK      (256 * 1024)
#define SERVER_ARENA      (16 * 1024 * 1024)
#define SERVER_REQ_MAX    (8 * 1024 * 1024)
#define SERVER_BODY_MAX   (8 * 1024 * 1024)
#define SERVER_RESP_MIN   (256 * 1024)
#define SERVER_TIMEOUT_S  5
#define SERVER_DEADLINE_S 10
#define SERVER_IDLE_S     5
#define SERVER_KEEPALIVE_MAX 100

void server_banner(void);
int server_run(int port);

#endif
