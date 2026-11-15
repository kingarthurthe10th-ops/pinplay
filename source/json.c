#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { const char *p; int depth; bool bad; } P;

static void ws(P *p) { while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n' || *p->p == '\r') p->p++; }

static int hex4(const char *s) {
	int v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i]; v <<= 4;
		if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
	}
	return v;
}

static int utf8(char *o, unsigned cp) {
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 63); return 2; }
	if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 63); o[2] = 0x80 | (cp & 63); return 3; }
	o[0] = 0xF0 | (cp >> 18); o[1] = 0x80 | ((cp >> 12) & 63); o[2] = 0x80 | ((cp >> 6) & 63); o[3] = 0x80 | (cp & 63); return 4;
}

static char *pstring(P *p) {            // p->p at the opening quote
	const char *s = ++p->p; int n = 0;
	while (s[n] && s[n] != '"') { if (s[n] == '\\' && s[n + 1]) n++; n++; }
	if (!s[n]) { p->bad = true; return NULL; }
	char *out = malloc(n + 1), *o = out;
	const char *e = s + n;
	while (s < e) {
		if (*s != '\\') { *o++ = *s++; continue; }
		s++;
		switch (*s) {
		case 'n': *o++ = '\n'; s++; break;
		case 't': *o++ = '\t'; s++; break;
		case 'r': *o++ = '\r'; s++; break;
		case 'b': *o++ = '\b'; s++; break;
		case 'f': *o++ = '\f'; s++; break;
		case 'u': {
			int cp = e - s >= 5 ? hex4(s + 1) : -1;
			if (cp < 0) { s++; break; }
			s += 5;
			if (cp >= 0xD800 && cp < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {
				int lo = hex4(s + 2);
				if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s += 6; }
			}
			o += utf8(o, cp);       // (a \u escape is 6 bytes, its UTF-8 at most 4, so it always fits)
			break;
		}
		default: *o++ = *s++;
		}
	}
	*o = 0;
	p->p = e + 1;
	return out;
}

static JNode *pvalue(P *p);

static JNode *pvalue(P *p) {
	ws(p);
	if (++p->depth > 64) { p->bad = true; return NULL; }
	JNode *n = calloc(1, sizeof *n);
	char c = *p->p;
	if (c == '{' || c == '[') {
		n->type = c == '{' ? J_OBJ : J_ARR;
		char close = c == '{' ? '}' : ']';
		p->p++; ws(p);
		JNode **tail = &n->child;
		if (*p->p == close) p->p++;
		else while (1) {
			char *key = NULL;
			if (n->type == J_OBJ) {
				ws(p);
				if (*p->p != '"') { p->bad = true; break; }
				key = pstring(p); if (p->bad) break;
				ws(p);
				if (*p->p != ':') { free(key); p->bad = true; break; }
				p->p++;
			}
			JNode *v = pvalue(p);
			if (!v) { free(key); p->bad = true; break; }
			v->key = key; *tail = v; tail = &v->next;
			if (p->bad) break;
			ws(p);
			if (*p->p == ',') { p->p++; continue; }
			if (*p->p == close) { p->p++; break; }
			p->bad = true; break;
		}
	} else if (c == '"') { n->type = J_STR; n->str = pstring(p); }
	else if (!strncmp(p->p, "true", 4)) { n->type = J_TRUE; p->p += 4; }
	else if (!strncmp(p->p, "false", 5)) { n->type = J_FALSE; p->p += 5; }
	else if (!strncmp(p->p, "null", 4)) { n->type = J_NULL; p->p += 4; }
	else if (c == '-' || (c >= '0' && c <= '9')) {
		char *end; n->type = J_NUM; n->num = strtod(p->p, &end);
		const char *start = p->p; p->p = end;
		int len = end - start; n->str = malloc(len + 1); memcpy(n->str, start, len); n->str[len] = 0;
	} else p->bad = true;
	p->depth--;
	return n;
}

JNode *json_parse(const char *text) {
	if (!text) return NULL;
	P p = { text, 0, false };
	JNode *n = pvalue(&p);
	if (p.bad) { json_free(n); return NULL; }
	return n;
}

void json_free(JNode *n) {
	while (n) {
		JNode *next = n->next;
		json_free(n->child);
		free(n->key); free(n->str); free(n);
		n = next;
	}
}

JNode *jget(const JNode *o, const char *key) {
	if (!o || o->type != J_OBJ) return NULL;
	for (JNode *c = o->child; c; c = c->next) if (c->key && !strcmp(c->key, key)) return c;
	return NULL;
}
bool jhas(const JNode *o, const char *key) { JNode *n = jget(o, key); return n && n->type != J_NULL; }
const char *jstr(const JNode *o, const char *key, const char *def) {
	JNode *n = jget(o, key);
	return n && (n->type == J_STR || n->type == J_NUM) && n->str ? n->str : def;
}
double jnum(const JNode *o, const char *key, double def) {
	JNode *n = jget(o, key);
	if (!n) return def;
	if (n->type == J_NUM) return n->num;
	if (n->type == J_STR && n->str && n->str[0]) return atof(n->str);
	if (n->type == J_TRUE) return 1;
	if (n->type == J_FALSE) return 0;
	return def;
}
bool jbool(const JNode *o, const char *key, bool def) {
	JNode *n = jget(o, key);
	if (!n) return def;
	if (n->type == J_TRUE) return true;
	if (n->type == J_FALSE || n->type == J_NULL) return false;
	if (n->type == J_NUM) return n->num != 0;
	return def;
}
int jlen(const JNode *a) { int k = 0; if (a && (a->type == J_ARR || a->type == J_OBJ)) for (JNode *c = a->child; c; c = c->next) k++; return k; }
JNode *jat(const JNode *a, int i) {
	if (!a || (a->type != J_ARR && a->type != J_OBJ)) return NULL;
	for (JNode *c = a->child; c; c = c->next) if (i-- == 0) return c;
	return NULL;
}

int json_esc(char *out, int cap, const char *s) {
	int n = 0;
#define PUT(ch) do { if (n < cap - 1) out[n] = (ch); n++; } while (0)
	PUT('"');
	for (; *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\') { PUT('\\'); PUT(c); }
		else if (c == '\n') { PUT('\\'); PUT('n'); }
		else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); for (char *q = b; *q; q++) PUT(*q); }
		else PUT(c);
	}
	PUT('"');
	if (cap > 0) out[n < cap ? n : cap - 1] = 0;
	return n;
#undef PUT
}
