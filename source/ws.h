#pragma once
#include "tls.h"

typedef struct WS WS;
WS *ws_open(const char *host, const char *path, volatile bool *abort, char *err, int errLen);
int ws_send(WS *w, const char *text, int len);           // <0 on error
int ws_recv(WS *w, char **msg, int timeoutMs);           // 1 = *msg holds a text message (valid until the next call), 0 timeout, -1 closed
void ws_close(WS *w);
