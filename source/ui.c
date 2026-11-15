#include "ui.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static C3D_RenderTarget *tTop, *tBot;
static C2D_TextBuf tbuf, mbuf;            // mbuf: only for measuring (cleared on every measure)
static bool topIsVideo;
static float screenW = 400;                 // width of the screen being drawn (for the scissor)
static u64 frameNo;

void ui_init(void) {
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
	C2D_Prepare();
	tTop = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	tBot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	tbuf = C2D_TextBufNew(8192); mbuf = C2D_TextBufNew(1024);
}

void ui_exit(void) {
	ui_thumbClear();
	C2D_TextBufDelete(tbuf); C2D_TextBufDelete(mbuf);
	C2D_Fini(); C3D_Fini();
}

void ui_begin(void) {
	topIsVideo = false; frameNo++;
	C2D_TextBufClear(tbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(tTop, C_BG); C2D_TargetClear(tBot, C_BG);
}
void ui_beginBottomOnly(void) {
	topIsVideo = true; frameNo++;
	C2D_TextBufClear(tbuf);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(tBot, C_BG);
}
void ui_top(void) { C2D_SceneBegin(tTop); screenW = 400; }
void ui_bottom(void) { C2D_SceneBegin(tBot); screenW = 320; }
void ui_end(void) { C3D_FrameEnd(0); }

void ui_rect(float x, float y, float w, float h, u32 c) { C2D_DrawRectSolid(x, y, 0.5f, w, h, c); }
void ui_circle(float x, float y, float r, u32 c) { C2D_DrawCircleSolid(x, y, 0.5f, r, c); }
void ui_tri(float x1, float y1, float x2, float y2, float x3, float y3, u32 c) {
	C2D_DrawTriangle(x1, y1, c, x2, y2, c, x3, y3, c, 0.5f);
}
void ui_round(float x, float y, float w, float h, float r, u32 c) {
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	if (r < 1) { ui_rect(x, y, w, h, c); return; }
	ui_rect(x + r, y, w - 2 * r, h, c);
	ui_rect(x, y + r, r, h - 2 * r, c);
	ui_rect(x + w - r, y + r, r, h - 2 * r, c);
	ui_circle(x + r, y + r, r, c); ui_circle(x + w - r, y + r, r, c);
	ui_circle(x + r, y + h - r, r, c); ui_circle(x + w - r, y + h - r, r, c);
}

float ui_textW(float size, const char *s) {
	C2D_TextBufClear(mbuf);
	C2D_Text t; C2D_TextParse(&t, mbuf, s);
	float w, h; C2D_TextGetDimensions(&t, size, size, &w, &h);
	return w;
}

float ui_text(float x, float y, float size, u32 c, int align, const char *s) {
	C2D_Text t; C2D_TextParse(&t, tbuf, s); C2D_TextOptimize(&t);
	float w, h; C2D_TextGetDimensions(&t, size, size, &w, &h);
	if (align == AL_CENTER) x -= w / 2; else if (align == AL_RIGHT) x -= w;
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, size, size, c);
	return w;
}

// Text too long for its space slides slowly back and forth (pause, scroll to the end, pause, back), clipped to the space.
void ui_textFit(float x, float y, float size, u32 c, int align, float maxW, const char *s) {
	float w = ui_textW(size, s);
	if (w <= maxW) { ui_text(x, y, size, c, align, s); return; }
	const float SPEED = 28, PAUSE = 1400;           // pixels per second, ms at each end
	float over = w - maxW, travel = over / SPEED * 1000, period = 2 * (PAUSE + travel);
	float t = (float)(osGetTime() % (u64)period), off;
	if (t < PAUSE) off = 0;
	else if (t < PAUSE + travel) off = (t - PAUSE) / travel * over;
	else if (t < 2 * PAUSE + travel) off = over;
	else off = over - (t - 2 * PAUSE - travel) / travel * over;
	float bx = align == AL_CENTER ? x - maxW / 2 : align == AL_RIGHT ? x - maxW : x;
	float h = 32 * size + 4, by = y - 2;
	// the render targets are the screens turned sideways: screen x runs down the framebuffer, screen y across it
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (by + h)), (u32)(screenW - (bx + maxW)), (u32)(240 - by), (u32)(screenW - bx));
	ui_text(bx - off, y, size, c, AL_LEFT, s);
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

void ui_spinner(float x, float y, float r, u32 c) {
	for (int i = 0; i < 8; i++) {
		float a = (i + frameNo / 4) * (float)M_PI / 4;
		u32 alpha = 40 + i * 26;
		ui_circle(x + cosf(a) * r, y + sinf(a) * r, r * 0.22f, (c & 0x00FFFFFF) | (alpha << 24));
	}
}

// ---- icons (x, y = top-left, s = size)
void icon_folder(float x, float y, float s, u32 c) {
	ui_round(x, y + s * 0.12f, s * 0.45f, s * 0.2f, s * 0.06f, c);
	ui_round(x, y + s * 0.22f, s, s * 0.68f, s * 0.08f, c);
}
void icon_film(float x, float y, float s, u32 c) {
	ui_round(x, y + s * 0.1f, s, s * 0.8f, s * 0.12f, c);
	for (int i = 0; i < 3; i++) {
		ui_rect(x + s * 0.08f, y + s * (0.2f + 0.22f * i), s * 0.1f, s * 0.12f, C_BG);
		ui_rect(x + s * 0.82f, y + s * (0.2f + 0.22f * i), s * 0.1f, s * 0.12f, C_BG);
	}
	ui_tri(x + s * 0.38f, y + s * 0.32f, x + s * 0.38f, y + s * 0.68f, x + s * 0.66f, y + s * 0.5f, C_BG);
}
void icon_download(float x, float y, float s, u32 c) {
	ui_rect(x + s * 0.42f, y, s * 0.16f, s * 0.5f, c);
	ui_tri(x + s * 0.2f, y + s * 0.42f, x + s * 0.8f, y + s * 0.42f, x + s * 0.5f, y + s * 0.75f, c);
	ui_rect(x + s * 0.1f, y + s * 0.84f, s * 0.8f, s * 0.14f, c);
}
void icon_play(float x, float y, float s, u32 c) { ui_tri(x + s * 0.2f, y, x + s * 0.2f, y + s, x + s, y + s * 0.5f, c); }
void icon_pause(float x, float y, float s, u32 c) {
	ui_rect(x + s * 0.15f, y, s * 0.25f, s, c); ui_rect(x + s * 0.6f, y, s * 0.25f, s, c);
}
void icon_back10(float x, float y, float s, u32 c, bool ahead) {
	if (ahead) { ui_tri(x, y, x, y + s, x + s * 0.5f, y + s * 0.5f, c); ui_tri(x + s * 0.5f, y, x + s * 0.5f, y + s, x + s, y + s * 0.5f, c); }
	else { ui_tri(x + s, y, x + s, y + s, x + s * 0.5f, y + s * 0.5f, c); ui_tri(x + s * 0.5f, y, x + s * 0.5f, y + s, x, y + s * 0.5f, c); }
}
void icon_stop(float x, float y, float s, u32 c) { ui_round(x + s * 0.1f, y + s * 0.1f, s * 0.8f, s * 0.8f, s * 0.12f, c); }
void icon_check(float x, float y, float s, u32 c) {
	ui_tri(x, y + s * 0.55f, x + s * 0.15f, y + s * 0.4f, x + s * 0.4f, y + s * 0.85f, c);
	ui_tri(x + s * 0.15f, y + s * 0.4f, x + s * 0.4f, y + s * 0.65f, x + s * 0.4f, y + s * 0.85f, c);
	ui_tri(x + s * 0.4f, y + s * 0.65f, x + s * 0.85f, y + s * 0.1f, x + s, y + s * 0.25f, c);
	ui_tri(x + s * 0.4f, y + s * 0.65f, x + s, y + s * 0.25f, x + s * 0.4f, y + s * 0.85f, c);
}
void icon_cross(float x, float y, float s, u32 c) {
	float t = s * 0.12f;
	C2D_DrawLine(x + t, y + t, c, x + s - t, y + s - t, c, s * 0.18f, 0.5f);
	C2D_DrawLine(x + s - t, y + t, c, x + t, y + s - t, c, s * 0.18f, 0.5f);
}
void icon_up(float x, float y, float s, u32 c) {
	ui_tri(x + s * 0.5f, y, x + s * 0.1f, y + s * 0.45f, x + s * 0.9f, y + s * 0.45f, c);
	ui_rect(x + s * 0.38f, y + s * 0.4f, s * 0.24f, s * 0.6f, c);
}

// ---- touch
static touchPosition tp; static bool down, wasDown; static float startX, startY, lastX, lastY;
static int fakeOn = -1; static float fakeX, fakeY;
void ui_fake(int d, float x, float y) { fakeOn = d; fakeX = x; fakeY = y; }     // (self-driving tests)
void ui_input(void) {
	wasDown = down;
	down = hidKeysHeld() & KEY_TOUCH;
	if (fakeOn >= 0) down = fakeOn;
	if (down) {
		if (fakeOn >= 0) { tp.px = (u16)fakeX; tp.py = (u16)fakeY; } else hidTouchRead(&tp);
		lastX = tp.px; lastY = tp.py;
		if (!wasDown) { startX = lastX; startY = lastY; }
	}
}
bool ui_touchDown(void) { return down; }
void ui_touchPos(float *x, float *y) { *x = lastX; *y = lastY; }
static bool inside(float px, float py, float x, float y, float w, float h) { return px >= x && px < x + w && py >= y && py < y + h; }
bool ui_touchIn(float x, float y, float w, float h) { return down && inside(lastX, lastY, x, y, w, h); }
bool ui_tapped(float x, float y, float w, float h) {
	return !down && wasDown && inside(lastX, lastY, x, y, w, h) && inside(startX, startY, x, y, w, h);
}

bool ui_button(float x, float y, float w, float h, const char *glyph, const char *label, bool primary, int ic) {
	bool held = ui_touchIn(x, y, w, h) && inside(startX, startY, x, y, w, h);
	u32 bg = primary ? (held ? UI_RGB(30, 130, 180) : C_ACCENT) : (held ? C_LINE : C_CARD2);
	ui_round(x, y, w, h, 10, bg);
	float cy = y + h / 2;
	if (ic != IC_NONE && (!label || !label[0])) {                         // icon-only button
		float s = h * 0.38f, ix = x + w / 2 - s / 2, iy = cy - s / 2 - (glyph ? 5 : 0);
		switch (ic) {
		case IC_PLAY: icon_play(ix, iy, s, C_TEXT); break;
		case IC_PAUSE: icon_pause(ix, iy, s, C_TEXT); break;
		case IC_BACK10: icon_back10(ix, iy, s, C_TEXT, false); break;
		case IC_AHEAD10: icon_back10(ix, iy, s, C_TEXT, true); break;
		case IC_STOP: icon_stop(ix, iy, s, C_TEXT); break;
		}
		if (glyph) ui_text(x + w / 2, y + h - 17, 0.5f, primary ? C_TEXT : C_MUTED, AL_CENTER, glyph);
	} else {
		float lw = ui_textW(0.55f, label), gw = glyph ? ui_textW(0.62f, glyph) + 5 : 0;
		float iw = ic != IC_NONE ? 20 : 0, tx = x + w / 2 - (lw + gw + iw) / 2;
		if (ic == IC_DOWNLOAD) icon_download(tx, cy - 8, 16, C_TEXT);
		else if (ic == IC_UP) icon_up(tx, cy - 8, 16, C_TEXT);
		else if (ic == IC_PLAY) icon_play(tx + 2, cy - 7, 14, C_TEXT);
		else if (ic == IC_FOLDER) icon_folder(tx, cy - 8, 16, C_TEXT);
		tx += iw;
		if (glyph) { ui_text(tx, cy - 9, 0.62f, primary ? C_TEXT : C_ACCENT2, AL_LEFT, glyph); tx += gw; }
		ui_text(tx, cy - 8, 0.55f, C_TEXT, AL_LEFT, label);
	}
	return ui_tapped(x, y, w, h);
}

void ui_buttonOff(float x, float y, float w, float h, const char *label, int ic) {
	ui_round(x, y, w, h, 10, C_CARD);                     // solid colours only: overlapping see-through pieces look patchy
	float cy = y + h / 2, lw = ui_textW(0.55f, label), iw = ic == IC_UP ? 20 : 0, tx = x + w / 2 - (lw + iw) / 2;
	if (ic == IC_UP) icon_up(tx, cy - 8, 16, C_LINE);
	ui_text(tx + iw, cy - 8, 0.55f, C_LINE, AL_LEFT, label);
}

// ---- preview picture
static C3D_Tex thumb; static bool thumbOk; static C2D_Image thumbImg; static Tex3DS_SubTexture thumbSub;
void ui_thumbClear(void) { if (thumbOk) { C3D_TexDelete(&thumb); thumbOk = false; } }
void ui_thumbSet(const u16 *px, int w, int h) {
	if (!thumbOk) { if (!C3D_TexInit(&thumb, 256, 128, GPU_RGB565)) return; thumbOk = true; C3D_TexSetFilter(&thumb, GPU_LINEAR, GPU_LINEAR); }
	u16 *dst = thumb.data;
	memset(dst, 0, 256 * 128 * 2);
	for (int y = 0; y < h && y < 128; y++)
		for (int x = 0; x < w && x < 256; x++) {
			u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
			dst[((y >> 3) * 32 + (x >> 3)) * 64 + m] = px[y * w + x];
		}
	GSPGPU_FlushDataCache(dst, 256 * 128 * 2);
	thumbSub = (Tex3DS_SubTexture){ (u16)w, (u16)h, 0.0f, 1.0f, w / 256.0f, 1.0f - h / 128.0f };
	thumbImg = (C2D_Image){ &thumb, &thumbSub };
}
bool ui_thumbDraw(float x, float y, float w, float h) {
	if (!thumbOk || !thumbImg.subtex) return false;
	C2D_DrawImageAt(thumbImg, x, y, 0.5f, NULL, w / thumbSub.width, h / thumbSub.height);
	return true;
}

void ui_shot(const char *name, const u16 *videoPic) {
	mkdir("sdmc:/pinplay_shots", 0777);
	char p[96]; snprintf(p, sizeof p, "sdmc:/pinplay_shots/%s.raw", name);
	FILE *f = fopen(p, "wb"); if (!f) return;
	u32 fmt = videoPic ? 2 : 1;                 // top: 1 = BGR8 framebuffer, 2 = RGB565 video picture
	fwrite("N3VS", 1, 4, f); fwrite(&fmt, 4, 1, f);
	if (videoPic) fwrite(videoPic, 2, 240 * 400, f);
	else { u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL); GSPGPU_InvalidateDataCache(fb, 240 * 400 * 3); fwrite(fb, 1, 240 * 400 * 3, f); }
	u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); GSPGPU_InvalidateDataCache(fb, 240 * 320 * 3);
	fwrite(fb, 1, 240 * 320 * 3, f);
	fclose(f);
}

// ---- word wrap
static int utf8len(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
int ui_wrap(const char *s, float size, float maxW, char lines[][160], int maxLines) {
	int n = 0; char cur[160] = ""; int cl = 0;
	const char *p = s;
	while (*p && n < maxLines) {
		while (*p == ' ') p++;
		if (!*p) break;
		const char *we = p; while (*we && *we != ' ') we += utf8len((unsigned char)*we);
		int wl = we - p;
		char trial[320];
		snprintf(trial, sizeof trial, "%s%s%.*s", cur, cl ? " " : "", wl, p);
		if (ui_textW(size, trial) <= maxW && (int)strlen(trial) < 159) { snprintf(cur, sizeof cur, "%s", trial); cl = strlen(cur); p = we; continue; }
		if (cl) { snprintf(lines[n++], 160, "%s", cur); cur[0] = 0; cl = 0; continue; }
		// one word wider than the line: break it by characters
		int k = 0; char part[160] = "";
		while (p + k < we) {
			int L = utf8len((unsigned char)p[k]);
			char t2[160]; snprintf(t2, sizeof t2, "%.*s", k + L, p);
			if (k && (ui_textW(size, t2) > maxW || k + L > 150)) break;
			snprintf(part, sizeof part, "%s", t2); k += L;
		}
		snprintf(lines[n++], 160, "%s", part); p += k;
	}
	if (cl && n < maxLines) snprintf(lines[n++], 160, "%s", cur);
	else if (cl && n == maxLines) {                       // ran out of lines: mark the cut
		int L = strlen(lines[n - 1]); if (L > 150) L = 150;
		snprintf(lines[n - 1] + L, 160 - L, "...");
	}
	return n;
}

float ui_textBox(float x, float y, float w, float h, float maxSize, float minSize, u32 c, int align, const char *s) {
	// wrapping costs a lot of measuring, so the result for each text/box is kept (a few at a time)
	typedef struct { u32 key; float size; int n; char lines[8][160]; } Wrap;
	static Wrap cache[6]; static int next;
	u32 key = 2166136261u;
	for (const char *p = s; *p; p++) key = (key ^ (u8)*p) * 16777619u;
	key ^= (u32)(w * 7 + h * 131 + maxSize * 1000 + minSize * 100000);
	Wrap *wr = NULL;
	for (int i = 0; i < 6; i++) if (cache[i].key == key && cache[i].n) { wr = &cache[i]; break; }
	if (!wr) {
		wr = &cache[next]; next = (next + 1) % 6;
		float size = maxSize; int n = 0;
		for (; size >= minSize; size -= 0.05f) {
			int maxL = (int)(h / (size * 30 + 2)); if (maxL < 1) maxL = 1; if (maxL > 8) maxL = 8;
			n = ui_wrap(s, size, w, wr->lines, maxL);
			bool cut = n == maxL && strstr(wr->lines[n - 1], "...");
			if (!cut || size - 0.05f < minSize) break;
		}
		wr->key = key; wr->size = size; wr->n = n ? n : 1;
		if (!n) wr->lines[0][0] = 0;
	}
	float size = wr->size, lh = size * 30 + 2, ty = y + (h - wr->n * lh) / 2;
	float tx = align == AL_CENTER ? x + w / 2 : align == AL_RIGHT ? x + w : x;
	for (int i = 0; i < wr->n; i++) ui_text(tx, ty + i * lh, size, c, align, wr->lines[i]);
	return size;
}
