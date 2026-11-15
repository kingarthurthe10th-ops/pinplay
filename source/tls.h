#pragma once
// TLS 1.2 connections on mbedTLS (the 3DS's own sslc is too old for kahoot.it), plus a small HTTPS GET.
#include <3ds.h>
#include <stdbool.h>

typedef struct Conn Conn;

bool tls_init(void);                 // sockets + sslc (random numbers) + CA bundle from romfs:/cacert.pem
void tls_exit(void);
const char *tls_initError(void);

// abort: set to true from another thread to give up quickly. err gets a short reason on failure.
Conn *conn_open(const char *host, int port, volatile bool *abort, char *err, int errLen);
int conn_write(Conn *c, const void *buf, int len);              // whole buffer; <0 on error
int conn_read(Conn *c, void *buf, int len, int timeoutMs);      // >0 bytes, 0 timeout, <0 closed/error
void conn_close(Conn *c);

typedef struct {
	int status;
	char sessionToken[2048];          // x-kahoot-session-token header
	char *body; int bodyLen;         // malloc'd, NUL-terminated
} HttpResp;
// GET https://host/path. Returns false on network failure (err says why); HTTP errors come back in status.
bool https_get(const char *host, const char *path, HttpResp *r, volatile bool *abort, char *err, int errLen);
void http_free(HttpResp *r);

extern const char *UA_STRING;
