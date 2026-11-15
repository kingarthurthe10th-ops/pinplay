#include "tls.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

const char *UA_STRING = "Mozilla/5.0 (Nintendo 3DS; U; ; en) Version/1.7630.US PinPlay/1.0";

#define SOC_SIZE 0x100000
static u32 *socBuf;
static bool socOk, sslcOk, tlsOk;
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt cas;
static mbedtls_ssl_config conf;
static char initErr[96];
// last session per host so the second connection (the websocket) can skip the full handshake
static mbedtls_ssl_session saved; static bool haveSaved; static char savedHost[64];
static LightLock saveLock;

struct Conn { int sock; mbedtls_ssl_context ssl; volatile bool *abort; bool sslInit; };

const char *tls_initError(void) { return initErr; }

bool tls_init(void) {
	LightLock_Init(&saveLock);
	if (!socBuf) socBuf = memalign(0x1000, SOC_SIZE);
	if (!socBuf) { snprintf(initErr, sizeof initErr, "out of memory"); return false; }
	socOk = R_SUCCEEDED(socInit(socBuf, SOC_SIZE));
	if (!socOk) { snprintf(initErr, sizeof initErr, "network service failed to start"); return false; }
	sslcOk = R_SUCCEEDED(sslcInit(0));       // mbedTLS gets its random numbers from sslc
	mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg); mbedtls_x509_crt_init(&cas); mbedtls_ssl_config_init(&conf);
	int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const u8 *)"PinPlay", 7);
	if (r) { snprintf(initErr, sizeof initErr, "random seed failed (-0x%x)", -r); return false; }
	FILE *f = fopen("romfs:/cacert.pem", "rb");
	if (!f) { snprintf(initErr, sizeof initErr, "romfs:/cacert.pem missing"); return false; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *pem = malloc(n + 1); fread(pem, 1, n, f); fclose(f); pem[n] = 0;
	r = mbedtls_x509_crt_parse(&cas, (const u8 *)pem, n + 1); free(pem);
	if (r < 0) { snprintf(initErr, sizeof initErr, "CA bundle bad (-0x%x)", -r); return false; }
	mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conf, &cas, NULL);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);   // TLS 1.2
	static const mbedtls_ecp_group_id curves[] = { MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_SECP384R1, MBEDTLS_ECP_DP_NONE };
	mbedtls_ssl_conf_curves(&conf, curves);      // fast curves first: the old 3DS's CPU is slow
	mbedtls_ssl_conf_read_timeout(&conf, 15000);
	tlsOk = true;
	return true;
}

void tls_exit(void) {
	if (haveSaved) mbedtls_ssl_session_free(&saved);
	if (tlsOk) { mbedtls_ssl_config_free(&conf); mbedtls_x509_crt_free(&cas); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy); }
	if (sslcOk) sslcExit();
	if (socOk) socExit();
}

// ---- TCP connect: a blocking connect in a detached thread, so a silently dropped connection can't freeze anything
typedef struct { LightLock lock; struct sockaddr_in sa; int state, sock, err; bool abandoned; } ConnJob;

static void connThread(void *arg) {
	ConnJob *j = arg; int err = 0;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) err = errno;
	else if (connect(s, (struct sockaddr *)&j->sa, sizeof j->sa) < 0) { err = errno ? errno : -1; close(s); s = -1; }
	LightLock_Lock(&j->lock);
	if (j->abandoned) { LightLock_Unlock(&j->lock); if (s >= 0) close(s); free(j); return; }
	j->sock = s; j->err = err; j->state = s >= 0 ? 1 : 2;
	LightLock_Unlock(&j->lock);
}

static int tcpConnect(const char *host, int port, volatile bool *abort, char *err, int errLen) {
	struct addrinfo hints, *res = NULL;
	memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
		snprintf(err, errLen, "Can't find %s. Check the internet connection.", host); return -1;
	}
	ConnJob *j = calloc(1, sizeof *j);
	LightLock_Init(&j->lock);
	j->sa = *(struct sockaddr_in *)res->ai_addr; j->sa.sin_port = htons(port);
	freeaddrinfo(res);
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (!threadCreate(connThread, j, 16 * 1024, prio, -2, true)) { free(j); snprintf(err, errLen, "thread failed"); return -1; }
	u64 t0 = osGetTime();
	while (1) {
		bool stop = (abort && *abort), late = osGetTime() - t0 > 10000;
		LightLock_Lock(&j->lock);
		int st = j->state, s = j->sock, e = j->err;
		if (!st && (stop || late)) j->abandoned = true;
		LightLock_Unlock(&j->lock);
		if (st == 1) { free(j); fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); return s; }
		if (st == 2) { free(j); snprintf(err, errLen, "Can't connect to %s (error %d).", host, e); return -1; }
		if (stop) { snprintf(err, errLen, "Cancelled."); return -1; }
		if (late) { snprintf(err, errLen, "%s didn't answer.", host); return -1; }
		svcSleepThread(20000000);
	}
}

// ---- mbedTLS socket callbacks (the socket is non-blocking; poll gives the timeouts)
static int bioSend(void *ctx, const unsigned char *b, size_t n) {
	Conn *c = ctx;
	for (int tries = 0; tries < 300; tries++) {          // up to ~15 s
		int r = send(c->sock, b, n, 0);
		if (r >= 0) return r;
		if (errno != EAGAIN && errno != EWOULDBLOCK) return MBEDTLS_ERR_NET_SEND_FAILED;
		if (c->abort && *c->abort) return MBEDTLS_ERR_NET_SEND_FAILED;
		struct pollfd p = { c->sock, POLLOUT, 0 }; poll(&p, 1, 50);
	}
	return MBEDTLS_ERR_NET_SEND_FAILED;
}
static int bioRecvTimeout(void *ctx, unsigned char *b, size_t n, uint32_t ms) {
	Conn *c = ctx;
	u64 t0 = osGetTime();
	while (1) {
		int r = recv(c->sock, b, n, 0);
		if (r > 0) return r;
		if (r == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
		if (errno != EAGAIN && errno != EWOULDBLOCK) return MBEDTLS_ERR_NET_RECV_FAILED;
		if (c->abort && *c->abort) return MBEDTLS_ERR_NET_RECV_FAILED;
		u64 el = osGetTime() - t0;
		if (ms && el >= ms) return MBEDTLS_ERR_SSL_TIMEOUT;
		int wait = 50; if (ms && ms - el < 50) wait = ms - el;
		struct pollfd p = { c->sock, POLLIN, 0 }; poll(&p, 1, wait);
	}
}

Conn *conn_open(const char *host, int port, volatile bool *abort, char *err, int errLen) {
	if (!tlsOk) { snprintf(err, errLen, "TLS not ready: %s", initErr); return NULL; }
	int s = tcpConnect(host, port, abort, err, errLen);
	if (s < 0) return NULL;
	Conn *c = calloc(1, sizeof *c);
	c->sock = s; c->abort = abort;
	mbedtls_ssl_init(&c->ssl); c->sslInit = true;
	int r = mbedtls_ssl_setup(&c->ssl, &conf);
	if (!r) r = mbedtls_ssl_set_hostname(&c->ssl, host);
	if (r) { snprintf(err, errLen, "TLS setup failed (-0x%x)", -r); conn_close(c); return NULL; }
	mbedtls_ssl_set_bio(&c->ssl, c, bioSend, NULL, bioRecvTimeout);
	LightLock_Lock(&saveLock);
	if (haveSaved && !strcmp(savedHost, host)) mbedtls_ssl_set_session(&c->ssl, &saved);
	LightLock_Unlock(&saveLock);
	u64 t0 = osGetTime();
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		char e[80]; mbedtls_strerror(r, e, sizeof e);
		u32 flags = mbedtls_ssl_get_verify_result(&c->ssl);
		if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
			snprintf(err, errLen, "%s's certificate didn't check out (flags %lx). Is the 3DS clock right?", host, (unsigned long)flags);
		else snprintf(err, errLen, "Secure connection to %s failed: %s", host, e);
		conn_close(c); return NULL;
	}
	plog("tls %s ok in %llu ms (%s, %s)", host, osGetTime() - t0, mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
	LightLock_Lock(&saveLock);
	if (haveSaved) mbedtls_ssl_session_free(&saved);
	mbedtls_ssl_session_init(&saved);
	haveSaved = mbedtls_ssl_get_session(&c->ssl, &saved) == 0;
	snprintf(savedHost, sizeof savedHost, "%s", host);
	LightLock_Unlock(&saveLock);
	return c;
}

int conn_write(Conn *c, const void *buf, int len) {
	const u8 *b = buf;
	while (len > 0) {
		int r = mbedtls_ssl_write(&c->ssl, b, len);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		if (r <= 0) return -1;
		b += r; len -= r;
	}
	return 0;
}

int conn_read(Conn *c, void *buf, int len, int timeoutMs) {
	mbedtls_ssl_conf_read_timeout(&conf, timeoutMs > 0 ? timeoutMs : 1);   // (shared config; only one thread reads at a time)
	while (1) {
		int r = mbedtls_ssl_read(&c->ssl, buf, len);
		if (r > 0) return r;
		if (r == MBEDTLS_ERR_SSL_TIMEOUT) return 0;
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
		return -1;
	}
}

void conn_close(Conn *c) {
	if (!c) return;
	if (c->sslInit) { mbedtls_ssl_close_notify(&c->ssl); mbedtls_ssl_free(&c->ssl); }
	if (c->sock >= 0) close(c->sock);
	free(c);
}

// ---- HTTPS GET (Connection: close, reads to the end)
static int hexval(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }

bool https_get(const char *host, const char *path, HttpResp *r, volatile bool *abort, char *err, int errLen) {
	memset(r, 0, sizeof *r);
	Conn *c = conn_open(host, 443, abort, err, errLen);
	if (!c) return false;
	char req[768];
	int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: application/json, */*\r\n"
		"Origin: https://kahoot.it\r\nReferer: https://kahoot.it/\r\nConnection: close\r\n\r\n", path, host, UA_STRING);
	if (conn_write(c, req, n) < 0) { snprintf(err, errLen, "Lost the connection to %s.", host); conn_close(c); return false; }
	int cap = 16384, len = 0; char *d = malloc(cap);
	while (1) {
		if (cap - len < 4096) { cap *= 2; if (cap > 512 * 1024) break; d = realloc(d, cap); }
		int got = conn_read(c, d + len, cap - len - 1, 15000);
		if (got <= 0) break;
		len += got;
	}
	conn_close(c);
	d[len] = 0;
	char *hend = strstr(d, "\r\n\r\n");
	if (!hend || sscanf(d, "HTTP/1.%*d %d", &r->status) != 1) { free(d); snprintf(err, errLen, "%s sent a bad reply.", host); return false; }
	*hend = 0;
	bool chunked = false;
	for (char *line = strstr(d, "\r\n"); line && line < hend; line = strstr(line + 2, "\r\n")) {
		char *l = line + 2;
		if (!strncasecmp(l, "x-kahoot-session-token:", 23)) {
			l += 23; while (*l == ' ') l++;
			int k = 0; while (l[k] && l[k] != '\r' && k < (int)sizeof r->sessionToken - 1) { r->sessionToken[k] = l[k]; k++; }
			r->sessionToken[k] = 0;
		} else if (!strncasecmp(l, "transfer-encoding:", 18) && strstr(l, "chunked") && strstr(l, "chunked") < strstr(l, "\r\n")) chunked = true;
	}
	char *body = hend + 4; int blen = len - (body - d);
	char *out = malloc(blen + 1); int olen = 0;
	if (chunked) {
		char *p = body, *end = body + blen;
		while (p < end) {
			int sz = 0, h; while (p < end && (h = hexval(*p)) >= 0) { sz = sz * 16 + h; p++; }
			char *nl = strstr(p, "\r\n"); if (!nl) break; p = nl + 2;
			if (sz == 0 || p + sz > end) break;
			memcpy(out + olen, p, sz); olen += sz; p += sz + 2;
		}
	} else { memcpy(out, body, blen); olen = blen; }
	out[olen] = 0;
	free(d);
	r->body = out; r->bodyLen = olen;
	return true;
}

void http_free(HttpResp *r) { free(r->body); r->body = NULL; }
