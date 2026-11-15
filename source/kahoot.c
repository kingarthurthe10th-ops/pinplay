#include "kahoot.h"
#include "tls.h"
#include "ws.h"
#include "json.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <mbedtls/base64.h>

#define HOST "kahoot.it"
#define QCAP 24

typedef struct Out { int id; char *content; struct Out *next; } Out;

typedef struct Session {
	LightLock lock;
	volatile bool abort;
	bool uiGone, done;              // uiGone: the UI let go (the thread frees it); done: the thread finished
	char pin[16], name[64];
	bool nameReady;
	KEvent q[QCAP]; int qHead, qLen;
	Out *out, **outTail;
	// network-thread-only state
	WS *ws; char path[1800];
	char clientId[64], cid[32];
	int msgId; long long ack; int lag; long long offset;
	bool handshook, connected, loginSent, joined, everJoined, namerator, team, twoStep;
	u64 lastRx;
} Session;

static Session *cur;                // the UI's session
static char lastNick[64];

static long long nowMs(void) { return (long long)osGetTime() - 2208988800000LL; }   // osGetTime counts from 1900

// ---- events (network thread -> UI)
static void push(Session *s, const KEvent *e) {
	LightLock_Lock(&s->lock);
	if (s->qLen == QCAP) { s->qHead = (s->qHead + 1) % QCAP; s->qLen--; }   // (drop the oldest; the UI reads every frame)
	s->q[(s->qHead + s->qLen) % QCAP] = *e; s->qLen++;
	LightLock_Unlock(&s->lock);
}
static void pushSimple(Session *s, KEvType t, int code, const char *text) {
	KEvent *e = calloc(1, sizeof *e);
	e->type = t; e->code = code;
	if (text) snprintf(e->text, sizeof e->text, "%s", text);
	push(s, e); free(e);
	if (t == KE_FAIL) plog("FAIL %d: %s", code, text ? text : "");
}
static void status(Session *s, const char *t) { pushSimple(s, KE_STATUS, 0, t); plog("status: %s", t); }

bool kahoot_poll(KEvent *ev) {
	Session *s = cur; if (!s) return false;
	bool got = false;
	LightLock_Lock(&s->lock);
	if (s->qLen) { *ev = s->q[s->qHead]; s->qHead = (s->qHead + 1) % QCAP; s->qLen--; got = true; }
	LightLock_Unlock(&s->lock);
	return got;
}

// ---- outgoing game messages (UI -> network thread)
static void queueMsg(int id, const char *content) {
	Session *s = cur; if (!s) return;
	Out *o = calloc(1, sizeof *o); o->id = id; o->content = strdup(content);
	LightLock_Lock(&s->lock);
	*s->outTail = o; s->outTail = &o->next;
	LightLock_Unlock(&s->lock);
}

void kahoot_answerChoice(int qi, const char *qt, int choice) {
	char b[160]; snprintf(b, sizeof b, "{\"type\":\"%s\",\"choice\":%d,\"questionIndex\":%d}", qt, choice, qi);
	queueMsg(45, b);
}
void kahoot_answerMulti(int qi, const char *qt, const int *c, int n) {
	char b[256]; int k = snprintf(b, sizeof b, "{\"type\":\"%s\",\"choice\":[", qt);
	for (int i = 0; i < n; i++) k += snprintf(b + k, sizeof b - k, "%s%d", i ? "," : "", c[i]);
	snprintf(b + k, sizeof b - k, "],\"questionIndex\":%d}", qi);
	queueMsg(45, b);
}
void kahoot_answerText(int qi, const char *qt, const char *text) {
	char esc[600]; json_esc(esc, sizeof esc, text);
	char b[800]; snprintf(b, sizeof b, "{\"type\":\"%s\",\"text\":%s,\"questionIndex\":%d}", qt, esc, qi);
	queueMsg(45, b);
}
void kahoot_twoStep(const int seq[4]) {
	Session *s = cur; if (!s) return;
	char b[128]; snprintf(b, sizeof b, "{\"gameId\":\"%s\",\"sequence\":[%d,%d,%d,%d]}", s->pin, seq[0], seq[1], seq[2], seq[3]);
	queueMsg(50, b);
}
void kahoot_setName(const char *name) {
	Session *s = cur; if (!s) return;
	LightLock_Lock(&s->lock);
	snprintf(s->name, sizeof s->name, "%s", name); s->nameReady = true;
	LightLock_Unlock(&s->lock);
	snprintf(lastNick, sizeof lastNick, "%s", name);
}
const char *kahoot_nickname(void) { return lastNick; }

// ---- the join puzzle: a quoted word and an arithmetic "offset" (kahoot.it evals it; we compute it)
typedef struct { const char *p; bool bad; } Calc;
// JS whitespace includes non-ASCII spaces (Kahoot pads the puzzle with ones like U+2003 and NBSP), so any byte >= 0x80 counts as space
static void cws(Calc *c) {
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r' || *c->p == '\v' || *c->p == '\f' || (unsigned char)*c->p >= 0x80) c->p++;
}
static double cAdd(Calc *c);
static double cAtom(Calc *c) {
	cws(c);
	if (*c->p == '(') { c->p++; double v = cAdd(c); cws(c); if (*c->p == ')') c->p++; else c->bad = true; return v; }
	if (*c->p == '-') { c->p++; return -cAtom(c); }
	if (*c->p == '+') { c->p++; return cAtom(c); }
	if (isdigit((unsigned char)*c->p) || *c->p == '.') { char *e; double v = strtod(c->p, &e); c->p = e; return v; }
	c->bad = true; return 0;
}
static double cMul(Calc *c) {
	double v = cAtom(c);
	while (!c->bad) {
		cws(c);
		if (*c->p == '*') { c->p++; v *= cAtom(c); }
		else if (*c->p == '/') { c->p++; v /= cAtom(c); }
		else if (*c->p == '%') { c->p++; v = fmod(v, cAtom(c)); }
		else break;
	}
	return v;
}
static double cAdd(Calc *c) {
	double v = cMul(c);
	while (!c->bad) {
		cws(c);
		if (*c->p == '+') { c->p++; v += cMul(c); }
		else if (*c->p == '-') { c->p++; v -= cMul(c); }
		else break;
	}
	return v;
}
static bool wordc(char ch) { return isalnum((unsigned char)ch) || ch == '_'; }

bool kahoot_solve(const char *ch, const char *tokB64, char *out, int outLen) {
	// message: the first '...' made of word characters (like kahoot.it's /'(\d*[a-z]*[A-Z]*)\w+'/)
	static char msg[2048]; static char expr[4096]; static u16 key[2048];     // (network thread only)
	int ml = 0;
	for (const char *p = ch; *p; p++) {
		if (*p != '\'') continue;
		const char *q = p + 1; while (wordc(*q)) q++;
		if (*q == '\'' && q - p - 1 >= 2) {
			ml = q - p - 1; if (ml >= (int)sizeof msg) ml = sizeof msg - 1;
			memcpy(msg, p + 1, ml); msg[ml] = 0; break;
		}
	}
	if (!ml) { plog("solve: no message in challenge"); return false; }
	// offset: between the first '=' and the next ';'
	const char *eq = strchr(ch, '='); if (!eq) { plog("solve: no '='"); return false; }
	const char *semi = strchr(eq, ';'); if (!semi) { plog("solve: no ';'"); return false; }
	int el = semi - eq - 1; if (el <= 0 || el >= (int)sizeof expr) { plog("solve: offset length %d", el); return false; }
	memcpy(expr, eq + 1, el); expr[el] = 0;
	Calc c = { expr, false };
	double off = cAdd(&c); cws(&c);
	if (c.bad || *c.p) { plog("solve: can't work out offset '%s' (stuck at '%.20s')", expr, c.p); return false; }
	for (int i = 0; i < ml; i++) {
		double v = fmod((unsigned char)msg[i] * (double)i + off, 77) + 48;     // JS: (code*pos + offset) % 77 + 48
		key[i] = (u16)(long long)v;                                         // String.fromCharCode: ToUint16
	}
	static u8 tok[1600]; size_t tl = 0;
	int r = mbedtls_base64_decode(tok, sizeof tok, &tl, (const u8 *)tokB64, strlen(tokB64));
	if (r != 0 || !tl) { plog("solve: bad token (-0x%x, %d chars)", -r, (int)strlen(tokB64)); return false; }
	if ((int)tl >= outLen) { plog("solve: session id too long (%d)", (int)tl); return false; }
	for (size_t i = 0; i < tl; i++) {
		int v = (tok[i] ^ key[i % ml]) & 0xFF;
		if (!isalnum(v)) { plog("solve: odd character %d in session id", v); }
		out[i] = (char)v;
	}
	out[tl] = 0;
	plog("solve: msg %d chars, offset %.0f -> session %.12s...", ml, off, out);
	return true;
}

// ---- Bayeux sending
static int sendRaw(Session *s, const char *json) {
	int n = strlen(json);
	plog(">> %.300s", json);
	return s->ws ? ws_send(s->ws, json, n) : -1;
}
static int sendHandshake(Session *s) {
	char b[512];
	snprintf(b, sizeof b, "[{\"id\":\"%d\",\"version\":\"1.0\",\"minimumVersion\":\"1.0\",\"channel\":\"/meta/handshake\","
		"\"supportedConnectionTypes\":[\"websocket\",\"long-polling\",\"callback-polling\"],\"advice\":{\"timeout\":60000,\"interval\":0},"
		"\"ext\":{\"ack\":true,\"timesync\":{\"tc\":%lld,\"l\":0,\"o\":0}}}]", ++s->msgId, nowMs());
	return sendRaw(s, b);
}
static int sendConnect(Session *s, bool first) {
	char b[512];
	if (first) snprintf(b, sizeof b, "[{\"id\":\"%d\",\"channel\":\"/meta/connect\",\"connectionType\":\"websocket\",\"advice\":{\"timeout\":0},"
		"\"clientId\":\"%s\",\"ext\":{\"ack\":0,\"timesync\":{\"tc\":%lld,\"l\":%d,\"o\":%lld}}}]", ++s->msgId, s->clientId, nowMs(), s->lag, s->offset);
	else snprintf(b, sizeof b, "[{\"id\":\"%d\",\"channel\":\"/meta/connect\",\"connectionType\":\"websocket\","
		"\"clientId\":\"%s\",\"ext\":{\"ack\":%lld,\"timesync\":{\"tc\":%lld,\"l\":%d,\"o\":%lld}}}]", ++s->msgId, s->clientId, s->ack, nowMs(), s->lag, s->offset);
	return sendRaw(s, b);
}
static int sendController(Session *s, const char *dataJson) {
	int n = strlen(dataJson) + 256; char *b = malloc(n);
	snprintf(b, n, "[{\"id\":\"%d\",\"channel\":\"/service/controller\",\"data\":%s,\"clientId\":\"%s\",\"ext\":{}}]", ++s->msgId, dataJson, s->clientId);
	int r = sendRaw(s, b); free(b); return r;
}
static int sendGameMsg(Session *s, int id, const char *content) {
	char esc[1600]; json_esc(esc, sizeof esc, content);
	char d[1800]; snprintf(d, sizeof d, "{\"gameid\":\"%s\",\"type\":\"message\",\"host\":\"" HOST "\",\"id\":%d,\"content\":%s}", s->pin, id, esc);
	return sendController(s, d);
}
static int sendLogin(Session *s) {
	char nm[160]; json_esc(nm, sizeof nm, s->name);
	char d[400];
	if (s->cid[0]) snprintf(d, sizeof d, "{\"type\":\"relogin\",\"gameid\":\"%s\",\"host\":\"" HOST "\",\"cid\":\"%s\",\"content\":\"{}\"}", s->pin, s->cid);
	else snprintf(d, sizeof d, "{\"type\":\"login\",\"gameid\":\"%s\",\"host\":\"" HOST "\",\"name\":%s,\"content\":\"{}\"}", s->pin, nm);
	s->loginSent = true;
	return sendController(s, d);
}

// ---- turning Kahoot's texts into plain lines (titles can carry simple markup)
static void plain(char *dst, int cap, const char *src) {
	int n = 0;
	for (const char *p = src ? src : ""; *p && n < cap - 1; ) {
		if (*p == '<') { const char *e = strchr(p, '>'); if (e) { p = e + 1; continue; } }
		if (*p == '&') {
			static const struct { const char *e; char c; } ents[] = { {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&nbsp;", ' '} };
			bool hit = false;
			for (unsigned i = 0; i < sizeof ents / sizeof *ents; i++)
				if (!strncmp(p, ents[i].e, strlen(ents[i].e))) { dst[n++] = ents[i].c; p += strlen(ents[i].e); hit = true; break; }
			if (hit) continue;
		}
		if (*p == '\n' || *p == '\r' || *p == '\t') { if (n && dst[n - 1] != ' ') dst[n++] = ' '; p++; continue; }
		dst[n++] = *p++;
	}
	while (n && dst[n - 1] == ' ') n--;
	dst[n] = 0;
}

static bool modeUnsupported(int id) {
	switch (id) {
	case 15: case 60: case 63: case 70: case 90: case 94: case 105: case 120: case 124: case 159: case 167: return true;
	}
	return false;
}

// ---- incoming
static void onPlayer(Session *s, int id, JNode *c) {
	KEvent *e = calloc(1, sizeof *e);
	switch (id) {
	case 135: sendGameMsg(s, 136, "null"); break;                         // ping -> pong
	case 14: {                                                             // name accepted
		const char *pn = jstr(c, "playerName", NULL);
		if (pn) { snprintf(s->name, sizeof s->name, "%s", pn); snprintf(lastNick, sizeof lastNick, "%s", pn); }
		break;
	}
	case 9: e->type = KE_QUIZ_START; push(s, e); break;
	case 1: case 2: {
		e->type = id == 1 ? KE_GET_READY : KE_QUESTION;
		e->index = (int)jnum(c, "gameBlockIndex", jnum(c, "questionIndex", 0));
		e->total = (int)jnum(c, "totalGameBlockCount", 0);
		snprintf(e->qtype, sizeof e->qtype, "%s", jstr(c, "type", jstr(c, "gameBlockType", "quiz")));
		snprintf(e->layout, sizeof e->layout, "%s", jstr(c, "layout", ""));
		if (id == 1) e->timeMs = jhas(c, "getReadyTimeAvailable") ? (int)jnum(c, "getReadyTimeAvailable", 5000)
		                     : jhas(c, "timeLeft") ? (int)(jnum(c, "timeLeft", 5) * 1000) : 5000;
		else e->timeMs = (int)jnum(c, "timeRemaining", jnum(c, "timeAvailable", 20000));     // ms left
		plain(e->title, sizeof e->title, jstr(c, "title", ""));
		JNode *ch = jget(c, "choices");
		int nc = (int)jnum(c, "numberOfChoices", jlen(ch));
		if (nc <= 0) nc = jlen(ch);
		if (nc > K_MAXCHOICES) nc = K_MAXCHOICES;
		e->numChoices = nc;
		for (int i = 0; i < nc && ch; i++) {
			JNode *o = jat(ch, i);
			const char *a = o ? jstr(o, "answer", NULL) : NULL;
			if (a && *a) { plain(e->choice[i], sizeof e->choice[i], a); e->hasChoiceText = true; }
		}
		e->allowed = (int)jnum(c, "numberOfAnswersAllowed", 1);
		push(s, e);
		break;
	}
	case 7: e->type = KE_TIME_UP; push(s, e); break;
	case 8: {
		e->type = KE_RESULT;
		e->hasAnswer = jbool(c, "hasAnswer", true);
		e->correct = jbool(c, "isCorrect", false);
		e->points = (int)jnum(c, "points", 0);
		JNode *pd = jget(c, "pointsData");
		if (pd) {
			e->points = (int)jnum(pd, "totalPointsWithBonuses", e->points);
			JNode *st = jget(pd, "answerStreakPoints");
			if (st) e->streak = (int)jnum(st, "streakLevel", 0);
		}
		e->score = (int)jnum(c, "totalScore", 0);
		e->rank = (int)jnum(c, "rank", 0);
		snprintf(e->qtype, sizeof e->qtype, "%s", jstr(c, "type", ""));
		JNode *nm = jget(c, "nemesis");
		if (nm && nm->type == J_OBJ) { plain(e->nemesis, sizeof e->nemesis, jstr(nm, "name", "")); e->nemesisScore = (int)jnum(nm, "totalScore", 0); }
		plain(e->answerText, sizeof e->answerText, jstr(c, "text", ""));
		push(s, e);
		break;
	}
	case 13: {
		e->type = KE_RANKING;
		e->rank = (int)jnum(c, "rank", 0); e->score = (int)jnum(c, "totalScore", 0);
		e->correctCount = (int)jnum(c, "correctCount", 0); e->incorrectCount = (int)jnum(c, "incorrectCount", 0);
		snprintf(e->medal, sizeof e->medal, "%s", jstr(c, "podiumMedalType", ""));
		push(s, e);
		break;
	}
	case 12: e->type = KE_FEEDBACK; push(s, e); break;
	case 5: e->type = KE_PLAY_AGAIN; push(s, e); break;
	case 30: e->type = jbool(c, "isPaused", jbool(c, "paused", true)) ? KE_PAUSE : KE_RESUME; push(s, e); break;
	case 20: e->type = KE_TEAM_TALK; e->timeMs = (int)jnum(c, "timeTalk", 5000); push(s, e); break;
	case 51: e->type = KE_TWOSTEP_WRONG; push(s, e); break;
	case 52: e->type = KE_TWOSTEP_OK; push(s, e); break;
	case 53: e->type = KE_TWOSTEP; push(s, e); break;
	case 10: {
		bool kicked = jhas(c, "kickCode");
		free(e);
		pushSimple(s, KE_FAIL, kicked ? KF_KICKED : KF_ENDED, kicked ? "The host removed you from the game." : "The host ended the game.");
		s->abort = true;
		return;
	}
	case 17: {                                                           // recovery data after joining / reconnecting
		int st = (int)jnum(c, "state", -1);
		JNode *d = jget(c, "data");
		plog("recovery state %d", st);
		if (st == 1 || st == 0) { e->type = KE_LOBBY; push(s, e); }
		else if (st == 3 && d) {
			e->type = KE_RANKING;
			e->rank = (int)jnum(d, "rank", 0); e->score = (int)jnum(d, "totalScore", 0);
			e->correctCount = (int)jnum(d, "correctCount", 0); e->incorrectCount = (int)jnum(d, "incorrectCount", 0);
			if (e->rank || e->score) push(s, e);
		}
		break;
	}
	default:
		if (modeUnsupported(id)) { e->type = KE_MODE_UNSUPPORTED; e->code = id; push(s, e); }
	}
	free(e);
}

static void onController(Session *s, JNode *d) {
	const char *type = jstr(d, "type", "");
	if (!strcmp(type, "loginResponse")) {
		const char *err = jstr(d, "error", NULL), *desc = jstr(d, "description", "");
		if (err) {
			plog("login refused: %s / %s", err, desc);
			if (!strcmp(err, "USER_INPUT") && strstr(desc, "uplicate")) pushSimple(s, KE_FAIL, KF_DUPNAME, "Someone in this game already has that nickname.");
			else if (!strcmp(err, "USER_INPUT")) { char t[200]; snprintf(t, sizeof t, "Kahoot didn't accept that nickname: %s", desc); pushSimple(s, KE_FAIL, KF_BADNAME, t); }
			else if (!strcmp(err, "NONEXISTING_SESSION")) pushSimple(s, KE_FAIL, KF_ENDED, s->everJoined ? "The game has ended." : "That game isn't running any more.");
			else { char t[200]; snprintf(t, sizeof t, "Kahoot refused the join (%s). %s", err, desc); pushSimple(s, KE_FAIL, KF_NET, t); }
			s->abort = true;
			return;
		}
		const char *cid = jstr(d, "cid", NULL);
		if (cid) snprintf(s->cid, sizeof s->cid, "%s", cid);
		bool again = s->everJoined;
		s->joined = s->everJoined = true;
		char rec[64]; snprintf(rec, sizeof rec, "{\"usingNamerator\":%s}", s->namerator ? "true" : "false");
		sendGameMsg(s, 16, rec);                                         // ask the host for the game's current state
		if (again) pushSimple(s, KE_RECONNECTED, 0, NULL);
		else {
			if (s->team) {                                                // team mode: a team of one
				char nm[160]; json_esc(nm, sizeof nm, s->name);
				char m[200]; snprintf(m, sizeof m, "[%s]", nm);
				sendGameMsg(s, 18, m);
			}
			pushSimple(s, KE_JOINED, 0, s->name);
			if (s->twoStep) pushSimple(s, KE_TWOSTEP, 0, NULL);
		}
	} else if (!strcmp(type, "status")) {
		const char *st = jstr(d, "status", "");
		plog("status %s", st);
		if (!strcmp(st, "LOCKED")) { pushSimple(s, KE_FAIL, KF_LOCKED, "The host has locked this game, so nobody new can join."); s->abort = true; }
		else if (!strcmp(st, "MISSING")) { pushSimple(s, KE_FAIL, KF_ENDED, "That game isn't running any more."); s->abort = true; }
		else if (!strcmp(st, "QUEUE")) status(s, "The game is full. Waiting in line...");
	}
}

// returns -1 when the connection must be dropped
static int onMessage(Session *s, const char *text) {
	plog("<< %.400s", text);
	JNode *root = json_parse(text);
	if (!root) { plog("bad json"); return 0; }
	int rc = 0;
	for (JNode *m = root->type == J_ARR ? root->child : root; m; m = root->type == J_ARR ? m->next : NULL) {
		const char *ch = jstr(m, "channel", "");
		JNode *ext = jget(m, "ext");
		if (!strcmp(ch, "/meta/handshake")) {
			if (!jbool(m, "successful", false)) { plog("handshake failed"); rc = -1; break; }
			snprintf(s->clientId, sizeof s->clientId, "%s", jstr(m, "clientId", ""));
			JNode *ts = jget(ext, "timesync");
			if (ts) {
				long long tc = (long long)jnum(ts, "tc", 0), sv = (long long)jnum(ts, "ts", 0), p = (long long)jnum(ts, "p", 0);
				s->lag = (int)((nowMs() - tc - p) / 2); s->offset = sv - tc - s->lag;
			}
			s->handshook = true;
			if (sendConnect(s, true) < 0) rc = -1;
		} else if (!strcmp(ch, "/meta/connect")) {
			if (ext && jhas(ext, "ack")) s->ack = (long long)jnum(ext, "ack", s->ack);
			if (!jbool(m, "successful", false)) {
				JNode *adv = jget(m, "advice");
				const char *re = jstr(adv, "reconnect", "retry");
				plog("connect not successful, advice %s", re);
				if (!strcmp(re, "none")) { rc = -1; break; }
				if (!strcmp(re, "handshake")) { s->handshook = s->connected = false; s->loginSent = false; s->joined = false; if (sendHandshake(s) < 0) rc = -1; continue; }
			}
			if (jbool(m, "successful", false)) s->connected = true;
			if (sendConnect(s, false) < 0) { rc = -1; break; }
		} else if (!strcmp(ch, "/service/player")) {
			JNode *d = jget(m, "data");
			if (d) {
				JNode *c = json_parse(jstr(d, "content", "null"));
				onPlayer(s, (int)jnum(d, "id", -1), c);
				json_free(c);
			}
		} else if (!strcmp(ch, "/service/controller") || !strcmp(ch, "/service/status")) {
			JNode *d = jget(m, "data");
			if (d) onController(s, d);
		}
		if (s->abort) break;
	}
	json_free(root);
	return rc;
}

// ---- reserve + websocket
// 1 ok, 0 retryable failure, -1 final failure (already reported)
static int reserve(Session *s, bool firstTime) {
	char path[96], err[200];
	snprintf(path, sizeof path, "/reserve/session/%s/?%lld", s->pin, nowMs());
	HttpResp r;
	if (!https_get(HOST, path, &r, &s->abort, err, sizeof err)) {
		if (s->abort) return -1;
		if (firstTime) pushSimple(s, KE_FAIL, KF_NET, err);
		plog("reserve failed: %s", err);
		return firstTime ? -1 : 0;
	}
	plog("reserve HTTP %d, %d bytes: %.300s", r.status, r.bodyLen, r.body);
	if (r.status == 404) {
		http_free(&r);
		pushSimple(s, KE_FAIL, s->everJoined ? KF_ENDED : KF_NOPIN, s->everJoined ? "The game has ended." : "There's no game with that PIN. Check it and try again.");
		return -1;
	}
	if (r.status != 200 || !r.sessionToken[0]) {
		char t[120]; snprintf(t, sizeof t, "Kahoot's server answered with an error (HTTP %d). Try again in a moment.", r.status);
		http_free(&r);
		if (firstTime) pushSimple(s, KE_FAIL, KF_NET, t);
		return firstTime ? -1 : 0;
	}
	JNode *j = json_parse(r.body);
	const char *challenge = jstr(j, "challenge", NULL);
	static char sid[1600];
	bool ok = challenge && kahoot_solve(challenge, r.sessionToken, sid, sizeof sid);
	if (!ok) {
		plog("challenge: %.3000s", challenge ? challenge : "(none)");
		json_free(j); http_free(&r);
		pushSimple(s, KE_FAIL, KF_UNSUPPORTED, "Couldn't solve Kahoot's join puzzle. Kahoot may have changed how joining works.");
		return -1;
	}
	if (firstTime) {
		s->twoStep = jbool(j, "twoFactorAuth", false);
		s->namerator = jbool(j, "namerator", false);
		const char *mode = jstr(j, "gameMode", jstr(j, "gameType", "normal"));
		s->team = !strcmp(mode, "team");
		plog("reserve: mode %s, twoStep %d, namerator %d, loginRequired %d, participantId %d", mode, s->twoStep, s->namerator,
			jbool(j, "loginRequired", false), jbool(j, "participantId", false));
		if (jbool(j, "loginRequired", false) || jbool(j, "participantId", false)) {
			json_free(j); http_free(&r);
			pushSimple(s, KE_FAIL, KF_UNSUPPORTED, "This game needs players to sign in to Kahoot or give an ID first. PinPlay can't do that.");
			return -1;
		}
		static const char *bad[] = { "marble", "sphere", "paintWar", "picasso", "submarine", "walk", "tapToCollect", "highestTowers", "puzzlebox", "temple", "collaboration", "stadium", NULL };
		for (int i = 0; bad[i]; i++) if (!strcmp(mode, bad[i])) {
			char t[200]; snprintf(t, sizeof t, "This game uses a special mode (%s) that PinPlay can't play.", mode);
			json_free(j); http_free(&r);
			pushSimple(s, KE_FAIL, KF_UNSUPPORTED, t);
			return -1;
		}
	}
	snprintf(s->path, sizeof s->path, "/cometd/%s/%s", s->pin, sid);
	json_free(j); http_free(&r);
	return 1;
}

static bool openSocket(Session *s) {
	char err[200];
	s->ws = ws_open(HOST, s->path, &s->abort, err, sizeof err);
	if (!s->ws) { plog("ws open failed: %s", err); return false; }
	s->handshook = s->connected = s->loginSent = s->joined = false;
	s->clientId[0] = 0; s->ack = 0;
	s->lastRx = osGetTime();
	return sendHandshake(s) >= 0;
}

static void dropSocket(Session *s) { if (s->ws) { ws_close(s->ws); s->ws = NULL; } }

static void flushOut(Session *s) {
	while (1) {
		LightLock_Lock(&s->lock);
		Out *o = s->joined ? s->out : NULL;
		if (o) { s->out = o->next; if (!s->out) s->outTail = &s->out; }
		LightLock_Unlock(&s->lock);
		if (!o) return;
		sendGameMsg(s, o->id, o->content);
		free(o->content); free(o);
	}
}

// keeps the connection going until it drops: 0 = dropped, -1 = finished
static int serve(Session *s) {
	while (!s->abort) {
		char *msg = NULL;
		int r = ws_recv(s->ws, &msg, 100);
		if (r < 0) { plog("socket closed"); return 0; }
		if (r > 0) { s->lastRx = osGetTime(); if (onMessage(s, msg) < 0) return 0; }
		if (s->abort) return -1;
		if (s->connected && !s->loginSent && s->nameReady && sendLogin(s) < 0) return 0;
		flushOut(s);
		if (osGetTime() - s->lastRx > 75000) { plog("nothing heard for 75 s"); return 0; }
	}
	return -1;
}

static void sessionThread(void *arg) {
	Session *s = arg;
	plog("session start pin %s name '%s'", s->pin, s->name);
	if (s->pin[0] == '0') {
		pushSimple(s, KE_FAIL, KF_UNSUPPORTED, "PINs that start with 0 are self-paced challenges. PinPlay only joins live games.");
		goto out;
	}
	status(s, "Finding the game...");
	if (reserve(s, true) <= 0) goto out;
	status(s, "Connecting securely...");
	if (!openSocket(s)) {
		if (!s->abort) pushSimple(s, KE_FAIL, KF_NET, "Couldn't open the game connection. Check the internet connection and try again.");
		goto out;
	}
	if (s->namerator) {
		LightLock_Lock(&s->lock); s->nameReady = false; LightLock_Unlock(&s->lock);
		pushSimple(s, KE_NAMERATOR, 0, NULL);
	}
	status(s, "Joining...");
	while (!s->abort) {
		int r = serve(s);
		dropSocket(s);
		if (r < 0 || s->abort) break;
		if (!s->everJoined) {
			pushSimple(s, KE_FAIL, KF_NET, "Lost the connection while joining. Try again.");
			break;
		}
		// reconnect: same game link first, then a fresh reservation; about a minute of tries
		pushSimple(s, KE_RECONNECTING, 0, NULL);
		u64 t0 = osGetTime(); bool back = false; int tries = 0;
		while (!s->abort && osGetTime() - t0 < 60000) {
			if (tries++ > 0) { for (int i = 0; i < 30 && !s->abort; i++) svcSleepThread(100000000); }
			if (tries > 1) { int rr = reserve(s, false); if (rr < 0) goto out; if (rr == 0) continue; }
			if (openSocket(s)) { back = true; break; }
			dropSocket(s);
		}
		if (!back) { if (!s->abort) pushSimple(s, KE_FAIL, KF_NET, "Lost the connection to the game and couldn't get back in."); break; }
	}
out:
	dropSocket(s);
	plog("session thread done");
	LightLock_Lock(&s->lock);
	s->done = true;
	bool gone = s->uiGone;
	LightLock_Unlock(&s->lock);
	if (gone) {
		while (s->out) { Out *o = s->out; s->out = o->next; free(o->content); free(o); }
		free(s);
	}
}

void kahoot_stop(void) {
	Session *s = cur; if (!s) return;
	cur = NULL;
	LightLock_Lock(&s->lock);
	s->abort = true; s->uiGone = true;
	bool done = s->done;
	LightLock_Unlock(&s->lock);
	if (done) {
		while (s->out) { Out *o = s->out; s->out = o->next; free(o->content); free(o); }
		free(s);
	}
}

void kahoot_start(const char *pin, const char *nick) {
	kahoot_stop();
	Session *s = calloc(1, sizeof *s);
	LightLock_Init(&s->lock);
	snprintf(s->pin, sizeof s->pin, "%s", pin);
	snprintf(s->name, sizeof s->name, "%s", nick);
	snprintf(lastNick, sizeof lastNick, "%s", nick);
	s->nameReady = true;
	s->outTail = &s->out;
	cur = s;
	s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (!threadCreate(sessionThread, s, 64 * 1024, prio - 1, -2, true)) {   // above the UI thread (see the Jellyfin 3DS notes)
		pushSimple(s, KE_FAIL, KF_NET, "Couldn't start the network thread.");
		s->done = true;
	}
}
