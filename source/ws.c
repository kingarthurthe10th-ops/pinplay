// A websocket client (RFC 6455, text messages only) on top of a TLS connection.
#include "ws.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <mbedtls/base64.h>

struct WS {
	Conn *c;
	u8 *buf; int len, cap;           // received bytes not used yet
	char *msg; int msgLen, msgCap;   // the text message being put together from fragments
	volatile bool *abort;
};

static int fill(WS *w, int need, int timeoutMs) {     // until len >= need: 1 ok, 0 timeout, -1 closed
	u64 t0 = osGetTime();
	while (w->len < need) {
		if (w->cap - w->len < 4096) {
			int nc = w->cap * 2; if (nc < need + 4096) nc = need + 4096;
			u8 *nb = realloc(w->buf, nc); if (!nb) return -1;
			w->buf = nb; w->cap = nc;
		}
		int left = timeoutMs - (int)(osGetTime() - t0);
		if (left <= 0) return 0;
		int r = conn_read(w->c, w->buf + w->len, w->cap - w->len - 1, left);     // (one byte kept free for a NUL)
		if (r < 0) return -1;
		if (r == 0) return 0;
		w->len += r;
	}
	return 1;
}

static void consume(WS *w, int n) { memmove(w->buf, w->buf + n, w->len - n); w->len -= n; }

static int sendFrame(WS *w, int op, const void *data, int n) {
	u8 hdr[14]; int h = 0;
	hdr[h++] = 0x80 | op;
	if (n < 126) hdr[h++] = 0x80 | n;
	else if (n < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = n >> 8; hdr[h++] = n; }
	else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[h++] = i < 4 ? (u8)((u32)n >> (i * 8)) : 0; }
	u32 rnd = (u32)osGetTime() * 2654435761u ^ svcGetSystemTick();
	u8 mask[4] = { rnd, rnd >> 8, rnd >> 16, rnd >> 24 };
	memcpy(hdr + h, mask, 4); h += 4;
	u8 *out = malloc(h + n); if (!out) return -1;
	memcpy(out, hdr, h);
	const u8 *d = data;
	for (int i = 0; i < n; i++) out[h + i] = d[i] ^ mask[i & 3];
	int r = conn_write(w->c, out, h + n);
	free(out);
	return r;
}

WS *ws_open(const char *host, const char *path, volatile bool *abort, char *err, int errLen) {
	Conn *c = conn_open(host, 443, abort, err, errLen);
	if (!c) return NULL;
	WS *w = calloc(1, sizeof *w);
	w->c = c; w->abort = abort;
	u8 key[16]; for (int i = 0; i < 16; i++) key[i] = (u8)(svcGetSystemTick() >> (i % 8)) ^ (u8)(i * 37);
	char k64[32]; size_t kl = 0; mbedtls_base64_encode((u8 *)k64, sizeof k64, &kl, key, 16); k64[kl] = 0;
	char req[3072];
	int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: https://kahoot.it\r\nUser-Agent: %s\r\n\r\n",
		path, host, k64, UA_STRING);
	if (conn_write(c, req, n) < 0) { snprintf(err, errLen, "Lost the connection while opening the game link."); ws_close(w); return NULL; }
	u64 t0 = osGetTime(); char *end = NULL;
	while (!end) {
		if (osGetTime() - t0 > 15000 || fill(w, w->len + 1, 15000) <= 0) { snprintf(err, errLen, "The game server didn't answer."); ws_close(w); return NULL; }
		w->buf[w->len] = 0;           // (fill always leaves room)
		end = strstr((char *)w->buf, "\r\n\r\n");
	}
	int status = 0; sscanf((char *)w->buf, "HTTP/1.%*d %d", &status);
	if (status != 101) {
		plog("ws upgrade refused: %.200s", (char *)w->buf);
		snprintf(err, errLen, "The game server refused the connection (HTTP %d).", status);
		ws_close(w); return NULL;
	}
	consume(w, end + 4 - (char *)w->buf);
	return w;
}

int ws_send(WS *w, const char *text, int len) { return sendFrame(w, 1, text, len); }

int ws_recv(WS *w, char **msg, int timeoutMs) {
	u64 t0 = osGetTime();
	while (1) {
		int left = timeoutMs - (int)(osGetTime() - t0); if (left < 1) left = 1;
		int r = fill(w, 2, left); if (r <= 0) return r;
		int b0 = w->buf[0], b1 = w->buf[1], h = 2;
		u64 n = b1 & 127;
		if (n == 126) { if ((r = fill(w, 4, 5000)) <= 0) return r ? r : -1; n = (w->buf[2] << 8) | w->buf[3]; h = 4; }
		else if (n == 127) { if ((r = fill(w, 10, 5000)) <= 0) return r ? r : -1; n = 0; for (int i = 0; i < 8; i++) n = (n << 8) | w->buf[2 + i]; h = 10; }
		if (b1 & 0x80) h += 4;                        // (servers don't mask, but be safe)
		if (n > 4 * 1024 * 1024) return -1;
		if ((r = fill(w, h + (int)n, 15000)) <= 0) return -1;
		u8 *pl = w->buf + h;
		if (b1 & 0x80) { u8 *m = w->buf + h - 4; for (u64 i = 0; i < n; i++) pl[i] ^= m[i & 3]; }
		int op = b0 & 15; bool fin = b0 & 0x80;
		if (op == 8) { plog("ws close from server (%d)", n >= 2 ? (pl[0] << 8 | pl[1]) : 0); consume(w, h + n); return -1; }
		if (op == 9) { sendFrame(w, 10, pl, (int)n); consume(w, h + n); continue; }     // ping -> pong
		if (op == 10) { consume(w, h + n); continue; }
		if (op == 1 || op == 2) w->msgLen = 0;
		if (w->msgLen + (int)n + 1 > w->msgCap) { w->msgCap = w->msgLen + n + 4096; w->msg = realloc(w->msg, w->msgCap); }
		memcpy(w->msg + w->msgLen, pl, n); w->msgLen += n; w->msg[w->msgLen] = 0;
		consume(w, h + n);
		if (fin) { *msg = w->msg; return 1; }
	}
}

void ws_close(WS *w) {
	if (!w) return;
	if (w->c) { u8 bye[2] = { 0x03, 0xE8 }; sendFrame(w, 8, bye, 2); conn_close(w->c); }
	free(w->buf); free(w->msg); free(w);
}
