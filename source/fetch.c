#include "fetch.h"
#include "tls.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
// JPEG pictures (Kahoot's image server sends JPEG even when asked for PNG): stb_image, public domain, JPEG only
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

struct Fetch {
	LightLock lock;
	int state; bool abandoned, decode;
	volatile bool abort;
	char host[128], path[1024];
	HttpResp r;
	u32 *px; int w, h;
};

static void decodeJpeg(Fetch *f) {
	int w, h, comp;
	if (!stbi_info_from_memory((const stbi_uc *)f->r.body, f->r.bodyLen, &w, &h, &comp)) { plog("jpeg: %s", stbi_failure_reason()); return; }
	if (w > 1024 || h > 1024) { plog("jpeg too big %dx%d", w, h); return; }
	stbi_uc *buf = stbi_load_from_memory((const stbi_uc *)f->r.body, f->r.bodyLen, &w, &h, &comp, 4);
	if (!buf) { plog("jpeg: %s", stbi_failure_reason()); return; }
	int n = w * h;
	u32 *px = malloc(n * 4);
	if (px) for (int i = 0; i < n; i++) px[i] = ((u32)buf[i * 4] << 24) | ((u32)buf[i * 4 + 1] << 16) | ((u32)buf[i * 4 + 2] << 8) | buf[i * 4 + 3];
	stbi_image_free(buf);
	if (px) { f->px = px; f->w = w; f->h = h; }
}

static void decodePng(Fetch *f) {
	if (f->r.bodyLen > 3 && (u8)f->r.body[0] == 0xFF && (u8)f->r.body[1] == 0xD8) { decodeJpeg(f); return; }
	png_image img; memset(&img, 0, sizeof img); img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&img, f->r.body, f->r.bodyLen)) { plog("png: %s", img.message); return; }
	img.format = PNG_FORMAT_RGBA;
	if (img.width > 1024 || img.height > 1024) { png_image_free(&img); plog("png too big %ux%u", img.width, img.height); return; }
	u8 *buf = malloc(PNG_IMAGE_SIZE(img));
	if (!buf) { png_image_free(&img); return; }
	if (!png_image_finish_read(&img, NULL, buf, 0, NULL)) { plog("png: %s", img.message); free(buf); return; }
	int n = img.width * img.height;
	u32 *px = malloc(n * 4);
	for (int i = 0; i < n; i++) px[i] = ((u32)buf[i * 4] << 24) | ((u32)buf[i * 4 + 1] << 16) | ((u32)buf[i * 4 + 2] << 8) | buf[i * 4 + 3];
	free(buf);
	f->px = px; f->w = img.width; f->h = img.height;
}

static void freeAll(Fetch *f) { http_free(&f->r); free(f->px); free(f); }

static void worker(void *arg) {
	Fetch *f = arg;
	char err[200];
	u64 t0 = osGetTime();
	bool ok = https_get(f->host, f->path, &f->r, &f->abort, err, sizeof err) && f->r.status == 200;
	if (ok && f->decode) { decodePng(f); ok = f->px != NULL; }
	plog("fetch %s%.80s: %s (HTTP %d, %d bytes, %llu ms)", f->host, f->path, ok ? "ok" : "failed", f->r.status, f->r.bodyLen, osGetTime() - t0);
	if (!ok && f->r.status == 0) plog("  %s", err);
	LightLock_Lock(&f->lock);
	bool gone = f->abandoned;
	f->state = ok ? FETCH_DONE : FETCH_FAILED;
	LightLock_Unlock(&f->lock);
	if (gone) freeAll(f);
}

Fetch *fetch_start(const char *url, bool decode) {
	if (strncmp(url, "https://", 8)) return NULL;
	Fetch *f = calloc(1, sizeof *f);
	LightLock_Init(&f->lock); f->decode = decode;
	const char *h = url + 8, *slash = strchr(h, '/');
	int hl = slash ? slash - h : (int)strlen(h); if (hl >= (int)sizeof f->host) hl = sizeof f->host - 1;
	memcpy(f->host, h, hl); f->host[hl] = 0;
	snprintf(f->path, sizeof f->path, "%s", slash ? slash : "/");
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (!threadCreate(worker, f, 48 * 1024, prio + 1, -2, true)) { free(f); return NULL; }   // below the UI and game threads
	return f;
}

int fetch_state(Fetch *f) { if (!f) return FETCH_FAILED; LightLock_Lock(&f->lock); int s = f->state; LightLock_Unlock(&f->lock); return s; }
const char *fetch_body(Fetch *f, int *len) { if (len) *len = f->r.bodyLen; return f->r.body; }
const u32 *fetch_pixels(Fetch *f, int *w, int *h) { *w = f->w; *h = f->h; return f->px; }

void fetch_free(Fetch *f) {
	if (!f) return;
	LightLock_Lock(&f->lock);
	bool running = f->state == FETCH_RUNNING;
	if (running) { f->abandoned = true; f->abort = true; }
	LightLock_Unlock(&f->lock);
	if (!running) freeAll(f);
}
