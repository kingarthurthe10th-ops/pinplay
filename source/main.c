// PinPlay: join a live Kahoot game from a 3DS by its game PIN.
// Top screen: the question and status. Touch screen: the answer tiles.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "ui.h"
#include "tls.h"
#include "kahoot.h"
#include "log.h"

#define VERSION "1.0.0"

// test builds: PP_SHOTS draws every screen from made-up events and saves screenshots (no network);
// PP_AUTOJOIN joins the game in sdmc:/pinplay_test.txt (PIN, nickname) and answers by itself, with screenshots.
#if defined(PP_SHOTS) || defined(PP_AUTOJOIN)
#define PP_TEST
#endif

typedef enum { SC_HOME, SC_CONNECT, SC_NAMERATOR, SC_LOBBY, SC_TWOSTEP, SC_QUIZSTART, SC_GETREADY, SC_QUESTION, SC_SENT,
	SC_TIMEUP, SC_RESULT, SC_RANKING, SC_FEEDBACK, SC_NOTYPE, SC_MODE, SC_FAIL } Screen;

static Screen sc = SC_HOME;
static char pin[16], nick[32];
static char statusText[256] = "";
static KEvent q, res, rk, fail;          // current question, last result, final ranking, failure
static bool haveResult, paused, reconnecting;
static int score, rank, sel = 0;
static u64 qStart, screenStart;          // when the question / screen started (ms)
static bool picked[K_MAXCHOICES]; static int order[K_MAXCHOICES], orderN, chosen = -1;
static char typed[64];
static int twoSeq[4], twoN; static bool twoWrong, twoSent;
static char genName[40]; static int spinsLeft;
static bool confirmLeave, quit;
static int questionsSeen;
static bool padUsed;                     // the D-pad highlight shows only once the D-pad has been used

// ---- colours and shapes of the answer tiles (the same colour/shape scheme players match on the host's screen)
#define CRGB(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))   // (a constant version of UI_RGB)
static const u32 TILE[K_MAXCHOICES] = { CRGB(226, 27, 60), CRGB(19, 104, 206), CRGB(216, 158, 0), CRGB(38, 137, 12), CRGB(11, 163, 165), CRGB(134, 76, 191) };
static const u32 TILE_DN[K_MAXCHOICES] = { CRGB(170, 20, 45), CRGB(14, 78, 155), CRGB(162, 118, 0), CRGB(28, 103, 9), CRGB(8, 122, 124), CRGB(100, 57, 143) };
#define C_WHITE UI_RGB(255, 255, 255)
#define C_DIM   UI_RGB(70, 76, 96)

static void shape(int k, float cx, float cy, float s, u32 c) {   // s = half size
	switch (k) {
	case 0: ui_tri(cx, cy - s, cx - s * 1.1f, cy + s * 0.85f, cx + s * 1.1f, cy + s * 0.85f, c); break;              // triangle
	case 1: ui_tri(cx, cy - s, cx - s, cy, cx + s, cy, c); ui_tri(cx - s, cy, cx + s, cy, cx, cy + s, c); break;     // diamond
	case 2: ui_circle(cx, cy, s * 0.95f, c); break;                                                                   // circle
	case 3: ui_rect(cx - s * 0.8f, cy - s * 0.8f, s * 1.6f, s * 1.6f, c); break;                                     // square
	case 4: for (int i = 0; i < 6; i++) { float a = i * 1.0472f, b = a + 1.0472f;                                    // hexagon
			ui_tri(cx, cy, cx + cosf(a) * s, cy + sinf(a) * s, cx + cosf(b) * s, cy + sinf(b) * s, c); } break;
	default: for (int i = 0; i < 5; i++) { float a = -1.5708f + i * 1.2566f, b = a + 0.6283f, d = a - 0.6283f;      // star
			ui_tri(cx, cy, cx + cosf(a) * s * 1.1f, cy + sinf(a) * s * 1.1f, cx + cosf(b) * s * 0.45f, cy + sinf(b) * s * 0.45f, c);
			ui_tri(cx, cy, cx + cosf(a) * s * 1.1f, cy + sinf(a) * s * 1.1f, cx + cosf(d) * s * 0.45f, cy + sinf(d) * s * 0.45f, c); } break;
	}
}

// true/false questions show "True" as the blue diamond and "False" as the red triangle
static int tileStyle(const KEvent *e, int i) {
	if (!strcmp(e->layout, "TRUE_FALSE") && e->numChoices == 2) return i == 0 ? 1 : 0;
	return i;
}

static const char *typeName(const char *t, const char *layout) {
	if (!strcmp(t, "quiz")) return !strcmp(layout, "TRUE_FALSE") ? "True or false" : "Quiz";
	if (!strcmp(t, "survey")) return "Poll";
	if (!strcmp(t, "open_ended")) return "Type answer";
	if (!strcmp(t, "word_cloud")) return "Word cloud";
	if (!strcmp(t, "multiple_select_quiz")) return "Pick all that are right";
	if (!strcmp(t, "multiple_select_poll")) return "Poll (pick several)";
	if (!strcmp(t, "jumble")) return "Puzzle: put in order";
	if (!strcmp(t, "content")) return "Slide";
	if (!strcmp(t, "slider")) return "Slider";
	if (!strcmp(t, "scale") || !strcmp(t, "nps")) return "Scale";
	if (!strcmp(t, "drop_pin") || !strcmp(t, "pin_it")) return "Pin answer";
	if (!strcmp(t, "brainstorming")) return "Brainstorm";
	if (!strcmp(t, "feedback")) return "Feedback";
	return t;
}
static bool isText(const char *t) { return !strcmp(t, "open_ended") || !strcmp(t, "word_cloud"); }
static bool isMulti(const char *t) { return !strncmp(t, "multiple_select", 15); }
static bool isJumble(const char *t) { return !strcmp(t, "jumble"); }
static bool isChoice(const char *t) { return !strcmp(t, "quiz") || !strcmp(t, "survey"); }
static bool playable(const char *t) { return isChoice(t) || isText(t) || isMulti(t) || isJumble(t); }
static bool isPoll(const char *t) { return !strcmp(t, "survey") || !strcmp(t, "multiple_select_poll") || !strcmp(t, "word_cloud"); }

static const char *ordinal(int n, char *b) {
	const char *suf = "th";
	if (n % 100 < 11 || n % 100 > 13) { if (n % 10 == 1) suf = "st"; else if (n % 10 == 2) suf = "nd"; else if (n % 10 == 3) suf = "rd"; }
	sprintf(b, "%d%s", n, suf); return b;
}

// ---- settings (the last nickname)
static void loadSettings(void) {
	FILE *f = fopen("sdmc:/3ds/pinplay/settings.txt", "r"); if (!f) return;
	char line[96];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "nick=", 5)) snprintf(nick, sizeof nick, "%s", line + 5);
	}
	fclose(f);
}
static void saveSettings(void) {
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/pinplay", 0777);
	FILE *f = fopen("sdmc:/3ds/pinplay/settings.txt", "w"); if (!f) return;
	fprintf(f, "nick=%s\n", nick); fclose(f);
}

// ---- software keyboard
static bool askText(char *out, int cap, const char *hint, bool numeric, int maxLen) {
	SwkbdState k; char buf[128];
	swkbdInit(&k, numeric ? SWKBD_TYPE_NUMPAD : SWKBD_TYPE_NORMAL, 2, maxLen);
	swkbdSetHintText(&k, hint);
	swkbdSetInitialText(&k, out);
	swkbdSetValidation(&k, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	if (!numeric) swkbdSetFeatures(&k, SWKBD_PREDICTIVE_INPUT);
	SwkbdButton b = swkbdInputText(&k, buf, sizeof buf);
	if (b != SWKBD_BUTTON_CONFIRM) return false;
	char *s = buf; while (*s == ' ') s++;
	int n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0;
	if (!n) return false;
	snprintf(out, cap, "%s", s);
	return true;
}

// ---- generated names for games where the host turns on random names (our own word lists)
static void makeName(void) {
	static const char *adj[] = { "Brave", "Sunny", "Swift", "Lucky", "Jolly", "Mighty", "Clever", "Cosmic", "Fuzzy", "Gentle", "Happy", "Bold",
		"Quiet", "Rapid", "Shiny", "Witty", "Zesty", "Calm", "Daring", "Eager", "Fancy", "Grand", "Noble", "Proud" };
	static const char *noun[] = { "Otter", "Falcon", "Panda", "Comet", "Maple", "Tiger", "Koala", "Pebble", "Rocket", "Walrus", "Badger", "Lynx",
		"Puffin", "Cactus", "Dolphin", "Heron", "Lemur", "Narwhal", "Owl", "Quokka", "Raven", "Salmon", "Turtle", "Yak" };
	u32 r = (u32)svcGetSystemTick();
	snprintf(genName, sizeof genName, "%s%s", adj[r % 24], noun[(r / 24) % 24]);
}

// ---- events from the game
static void go(Screen s) { sc = s; screenStart = osGetTime(); sel = 0; confirmLeave = false; padUsed = false; }

static void resetAnswer(void) { memset(picked, 0, sizeof picked); orderN = 0; chosen = -1; typed[0] = 0; }

static void startJoin(void);

static void handle(const KEvent *e) {
	switch (e->type) {
	case KE_STATUS: snprintf(statusText, sizeof statusText, "%s", e->text); break;
	case KE_FAIL: fail = *e; reconnecting = false; paused = false; go(SC_FAIL); break;
	case KE_NAMERATOR: makeName(); spinsLeft = 2; go(SC_NAMERATOR); break;
	case KE_JOINED: snprintf(nick, sizeof nick, "%s", e->text); go(SC_LOBBY); break;
	case KE_LOBBY: if (sc == SC_CONNECT) go(SC_LOBBY); break;
	case KE_TWOSTEP: twoN = 0; twoSent = false; go(SC_TWOSTEP); break;
	case KE_TWOSTEP_WRONG: twoN = 0; twoSent = false; twoWrong = true; go(SC_TWOSTEP); break;
	case KE_TWOSTEP_OK: twoWrong = false; go(SC_LOBBY); break;
	case KE_QUIZ_START: score = 0; rank = 0; haveResult = false; questionsSeen = 0; go(SC_QUIZSTART); break;
	case KE_GET_READY: q = *e; q.title[0] = 0; qStart = osGetTime(); go(SC_GETREADY); break;
	case KE_QUESTION:
		q = *e; qStart = osGetTime(); resetAnswer(); questionsSeen++;
		go(!strcmp(q.qtype, "content") ? SC_NOTYPE : playable(q.qtype) ? SC_QUESTION : SC_NOTYPE);
		break;
	case KE_TIME_UP: if (sc == SC_QUESTION || sc == SC_GETREADY) go(SC_TIMEUP); break;
	case KE_RESULT:
		res = *e; haveResult = true;
		if (e->score || e->rank) { score = e->score; rank = e->rank; }
		go(SC_RESULT);
		break;
	case KE_RANKING: rk = *e; score = e->score; rank = e->rank; go(SC_RANKING); break;
	case KE_FEEDBACK: if (sc != SC_RANKING) go(SC_FEEDBACK); break;
	case KE_PLAY_AGAIN: score = 0; rank = 0; haveResult = false; go(SC_LOBBY); break;
	case KE_PAUSE: paused = true; break;
	case KE_RESUME: paused = false; break;
	case KE_TEAM_TALK: snprintf(statusText, sizeof statusText, "Team talk! Discuss the answer with your team."); break;
	case KE_RECONNECTING: reconnecting = true; break;
	case KE_RECONNECTED: reconnecting = false; break;
	case KE_MODE_UNSUPPORTED: q.code = e->code; go(SC_MODE); break;
	}
}

static void leaveGame(void) { kahoot_stop(); reconnecting = paused = false; go(SC_HOME); }

static void startJoin(void) {
	statusText[0] = 0; reconnecting = paused = false; twoWrong = false;
	score = rank = 0; haveResult = false;
	kahoot_start(pin, nick);
	go(SC_CONNECT);
}

static void sendChoice(int i) {
	if (!strcmp(q.qtype, "jumble") || isMulti(q.qtype)) return;
	chosen = i;
	kahoot_answerChoice(q.index, q.qtype, i);
	go(SC_SENT);
}

// ---- drawing helpers
static void header(const char *left, const char *right) {
	ui_rect(0, 0, 400, 26, C_HEAD);
	ui_textFit(10, 5, 0.55f, C_TEXT, AL_LEFT, 250, left);
	if (right) ui_textFit(392, 5, 0.5f, C_MUTED, AL_RIGHT, 140, right);
}
static void footer(void) {
	ui_rect(0, 216, 400, 24, C_HEAD);
	ui_textFit(10, 220, 0.5f, C_TEXT, AL_LEFT, 220, nick);
	char b[64], o[16];
	if (rank > 0) snprintf(b, sizeof b, "%d pts  -  %s", score, ordinal(rank, o)); else snprintf(b, sizeof b, "%d pts", score);
	ui_text(392, 220, 0.5f, C_ACCENT2, AL_RIGHT, b);
}
static void logo(float x, float y, float s) {          // PinPlay's own mark: a map-pin with a play triangle in it
	ui_circle(x, y, s, C_ACCENT);
	ui_tri(x - s * 0.72f, y + s * 0.62f, x + s * 0.72f, y + s * 0.62f, x, y + s * 1.7f, C_ACCENT);
	ui_tri(x - s * 0.32f, y - s * 0.45f, x - s * 0.32f, y + s * 0.45f, x + s * 0.48f, y, C_WHITE);
}
static void bigMessage(const char *title, const char *msg, u32 titleColor) {
	ui_textBox(20, 40, 360, 50, 1.0f, 0.6f, titleColor, AL_CENTER, title);
	if (msg) ui_textBox(24, 96, 352, 110, 0.6f, 0.45f, C_TEXT, AL_CENTER, msg);
}
static void timerBar(float y) {
	if (q.timeMs <= 0) return;
	int left = q.timeMs - (int)(osGetTime() - qStart); if (left < 0) left = 0;
	float f = (float)left / q.timeMs;
	ui_round(20, y, 300, 10, 5, C_CARD2);
	ui_round(20, y, 300 * f, 10, 5, f > 0.25f ? C_ACCENT : C_BAD);
	char b[16]; snprintf(b, sizeof b, "%d", (left + 999) / 1000);
	ui_circle(352, y + 5, 16, C_CARD2);
	ui_text(352, y - 5, 0.6f, C_TEXT, AL_CENTER, b);
}

// a coloured answer tile; returns true when tapped (or chosen with the buttons)
static bool tile(int i, float x, float y, float w, float h, const char *text, bool on, bool dim, int badge) {
	int st = tileStyle(&q, i);
	bool held = ui_touchIn(x, y, w, h);
	u32 c = dim ? C_DIM : (held ? TILE_DN[st] : TILE[st]);
	ui_round(x, y, w, h, 8, c);
	if (sel == i && !dim && padUsed) { ui_round(x - 2, y - 2, w + 4, 3, 1, C_WHITE); ui_round(x - 2, y + h - 1, w + 4, 3, 1, C_WHITE);
		ui_rect(x - 2, y, 3, h, C_WHITE); ui_rect(x + w - 1, y, 3, h, C_WHITE); }
	if (text && *text) {
		shape(st, x + 16, y + 16, 9, C_WHITE);
		ui_textBox(x + 8, y + 28, w - 16, h - 32, 0.55f, 0.38f, C_WHITE, AL_CENTER, text);
	} else {
		float s = (w < h ? w : h) * 0.24f; if (s > 34) s = 34;
		shape(st, x + w / 2, y + h / 2, s, C_WHITE);
	}
	if (on) { ui_circle(x + w - 16, y + 16, 11, C_WHITE); icon_check(x + w - 24, y + 8, 16, TILE[st]); }
	if (badge > 0) { char b[4]; snprintf(b, sizeof b, "%d", badge); ui_circle(x + w - 16, y + 16, 11, C_WHITE); ui_text(x + w - 16, y + 7, 0.55f, TILE[st], AL_CENTER, b); }
	return !dim && ui_tapped(x, y, w, h);
}

// lays out n tiles in the area; returns the tapped one or -1. A presses the highlighted one, the D-pad moves it.
static int tiles(const KEvent *e, float top, float bottom, const bool *on, const int *badges, const bool *dim) {
	int n = e->numChoices; if (n < 1) n = 4;
	int cols = 2, rows = (n + 1) / 2;
	float gap = 6, W = (320 - gap * 3) / cols, H = (bottom - top - gap * (rows + 1)) / rows;
	u32 k = hidKeysDown();
	if (k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) { if (!padUsed) { padUsed = true; k &= ~(u32)(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT); } }
	if (k & KEY_RIGHT) sel = sel % 2 == 0 && sel + 1 < n ? sel + 1 : sel;
	if (k & KEY_LEFT) sel = sel % 2 == 1 ? sel - 1 : sel;
	if (k & KEY_DOWN) sel = sel + 2 < n ? sel + 2 : sel;
	if (k & KEY_UP) sel = sel - 2 >= 0 ? sel - 2 : sel;
	int hit = -1;
	for (int i = 0; i < n; i++) {
		float x = gap + (i % 2) * (W + gap), y = top + gap + (i / 2) * (H + gap), w = W;
		if (n % 2 == 1 && i == n - 1) { x = gap; w = 320 - gap * 2; }           // odd count: the last tile is full width
		if (n == 2) { x = gap + i * (W + gap); y = top + gap; H = bottom - top - gap * 2; }
		if (tile(i, x, y, w, H, e->hasChoiceText ? e->choice[i] : NULL, on && on[i], dim && dim[i], badges ? badges[i] : 0)) hit = i;
	}
	if ((k & KEY_A) && padUsed && !(dim && dim[sel])) hit = sel;
	return hit;
}

// ---- test-build helpers
#ifdef PP_TEST
static int shotNo;
static void shotNow(const char *name) { char b[64]; snprintf(b, sizeof b, "%02d_%s", shotNo++, name); ui_shot(b, NULL); plog("shot %s", b); }
#endif

// ---- one frame
static void frame(void) {
	ui_begin();
	u32 k = hidKeysDown();
	char b[256], o[16];

	// ===== top screen
	ui_top();
	switch (sc) {
	case SC_HOME:
		logo(200, 62, 30);
		ui_text(200, 122, 1.0f, C_TEXT, AL_CENTER, "PinPlay");
		ui_text(200, 156, 0.55f, C_MUTED, AL_CENTER, "Join a live Kahoot game with its game PIN.");
		ui_text(200, 178, 0.5f, C_MUTED, AL_CENTER, "Tap the boxes below to type the PIN and your nickname.");
		ui_text(392, 222, 0.42f, C_LINE, AL_RIGHT, "v" VERSION "  -  START: exit");
		break;
	case SC_CONNECT:
		header("Joining game", pin);
		ui_spinner(200, 95, 22, C_ACCENT2);
		ui_textBox(30, 130, 340, 60, 0.6f, 0.45f, C_TEXT, AL_CENTER, statusText[0] ? statusText : "Starting...");
		break;
	case SC_NAMERATOR:
		header("Pick a generated name", pin);
		ui_textBox(30, 34, 340, 50, 0.55f, 0.45f, C_MUTED, AL_CENTER, "The host has turned on random names for this game, so you can't type your own.");
		ui_textBox(20, 100, 360, 60, 1.0f, 0.6f, C_TEXT, AL_CENTER, genName);
		break;
	case SC_LOBBY:
		header("You're in!", pin);
		ui_text(200, 50, 0.6f, C_MUTED, AL_CENTER, "See your nickname on the host's screen?");
		ui_textBox(20, 80, 360, 60, 1.1f, 0.6f, C_TEXT, AL_CENTER, nick);
		ui_text(200, 160, 0.55f, C_MUTED, AL_CENTER, "Waiting for the host to start the game...");
		footer();
		break;
	case SC_TWOSTEP:
		header("Two-step join", pin);
		ui_textBox(24, 36, 352, 70, 0.6f, 0.45f, C_TEXT, AL_CENTER, twoWrong ? "That wasn't the right order. Look at the host's screen and try again."
			: "Tap the four shapes in the same order as on the host's screen.");
		for (int i = 0; i < 4; i++) {
			float x = 200 - 75 + i * 50;
			if (i < twoN) { ui_round(x - 20, 130, 40, 40, 8, TILE[twoSeq[i]]); shape(twoSeq[i], x, 150, 12, C_WHITE); }
			else ui_round(x - 20, 130, 40, 40, 8, C_CARD2);
		}
		if (twoSent) ui_text(200, 186, 0.55f, C_MUTED, AL_CENTER, "Checking...");
		break;
	case SC_QUIZSTART:
		header("Get ready!", NULL);
		ui_textBox(20, 70, 360, 80, 1.0f, 0.6f, C_TEXT, AL_CENTER, "The game is starting");
		footer();
		break;
	case SC_GETREADY:
		snprintf(b, sizeof b, q.total ? "Question %d of %d" : "Question %d", q.index + 1, q.total);
		header(b, typeName(q.qtype, q.layout));
		ui_textBox(20, 50, 360, 60, 1.0f, 0.6f, C_TEXT, AL_CENTER, "Get ready...");
		{ int left = q.timeMs - (int)(osGetTime() - qStart); if (left < 0) left = 0;
		  snprintf(b, sizeof b, "%d", (left + 999) / 1000); ui_circle(200, 150, 28, C_ACCENT); ui_text(200, 133, 1.0f, C_WHITE, AL_CENTER, b); }
		footer();
		break;
	case SC_QUESTION: case SC_SENT: case SC_TIMEUP: case SC_NOTYPE:
		snprintf(b, sizeof b, q.total ? "Question %d of %d" : "Question %d", q.index + 1, q.total);
		header(b, typeName(q.qtype, q.layout));
		if (q.title[0]) ui_textBox(16, 32, 368, 140, 0.8f, 0.42f, C_TEXT, AL_CENTER, q.title);
		else ui_textBox(16, 32, 368, 140, 0.65f, 0.45f, C_MUTED, AL_CENTER, "Look at the host's screen for the question.");
		if (sc == SC_SENT) ui_text(200, 178, 0.6f, C_ACCENT2, AL_CENTER, isPoll(q.qtype) ? "Vote sent! Waiting for the others..." : "Answer sent! Waiting for the result...");
		else if (sc == SC_TIMEUP) ui_text(200, 178, 0.6f, C_BAD, AL_CENTER, "Time's up!");
		else if (sc == SC_QUESTION) timerBar(186);
		footer();
		break;
	case SC_RESULT: {
		bool poll = isPoll(res.qtype) || isPoll(q.qtype);
		u32 bg = !res.hasAnswer ? C_CARD2 : poll ? C_ACCENT : res.correct ? C_OK : C_BAD;
		ui_rect(0, 0, 400, 216, bg);
		const char *t = !res.hasAnswer ? "Time's up" : poll ? "Thanks for voting!" : res.correct ? "Correct!" : "Incorrect";
		ui_text(200, 30, 1.2f, C_WHITE, AL_CENTER, t);
		if (res.hasAnswer && res.correct) {
			snprintf(b, sizeof b, "+ %d", res.points); ui_round(130, 78, 140, 40, 10, UI_RGB(48, 150, 92));
			ui_text(200, 84, 0.9f, C_WHITE, AL_CENTER, b);
		}
		if (res.streak >= 2) { snprintf(b, sizeof b, "Answer streak: %d", res.streak); ui_text(200, 126, 0.55f, C_WHITE, AL_CENTER, b); }
		if (res.answerText[0] && !poll && !res.correct && isText(q.qtype)) { snprintf(b, sizeof b, "Your answer: %s", res.answerText); ui_textBox(20, 126, 360, 24, 0.5f, 0.4f, C_WHITE, AL_CENTER, b); }
		if (rank > 0) { snprintf(b, sizeof b, "You're in %s place", ordinal(rank, o)); ui_text(200, 152, 0.65f, C_WHITE, AL_CENTER, b); }
		if (res.nemesis[0] && res.nemesisScore > score) { snprintf(b, sizeof b, "%d points behind %s", res.nemesisScore - score, res.nemesis); ui_textBox(20, 180, 360, 26, 0.5f, 0.4f, C_WHITE, AL_CENTER, b); }
		footer();
		break;
	}
	case SC_RANKING: {
		header("Game over", NULL);
		u32 mc = !strcmp(rk.medal, "gold") ? UI_RGB(240, 196, 60) : !strcmp(rk.medal, "silver") ? UI_RGB(200, 205, 215) : !strcmp(rk.medal, "bronze") ? UI_RGB(205, 127, 50) : 0;
		if (!mc && rank >= 1 && rank <= 3) mc = rank == 1 ? UI_RGB(240, 196, 60) : rank == 2 ? UI_RGB(200, 205, 215) : UI_RGB(205, 127, 50);
		if (mc) { ui_circle(200, 78, 34, mc); ui_text(200, 58, 1.1f, C_BG, AL_CENTER, rank ? ordinal(rank, o) : "!"); }
		else if (rank) { ui_circle(200, 78, 34, C_CARD2); ui_text(200, 60, 1.0f, C_TEXT, AL_CENTER, ordinal(rank, o)); }
		snprintf(b, sizeof b, rank ? "You finished %s!" : "Thanks for playing!", ordinal(rank, o));
		ui_text(200, 122, 0.8f, C_TEXT, AL_CENTER, b);
		snprintf(b, sizeof b, "%d points", score); ui_text(200, 152, 0.65f, C_ACCENT2, AL_CENTER, b);
		if (rk.correctCount || rk.incorrectCount) { snprintf(b, sizeof b, "%d right, %d wrong", rk.correctCount, rk.incorrectCount); ui_text(200, 180, 0.55f, C_MUTED, AL_CENTER, b); }
		footer();
		break;
	}
	case SC_FEEDBACK:
		header("Game over", NULL);
		bigMessage("Thanks for playing!", "The host is collecting feedback. Your final place will show up here.", C_TEXT);
		footer();
		break;
	case SC_MODE:
		header("Special game mode", pin);
		bigMessage("Can't play this part", "The host started a game mode PinPlay doesn't support. You stay in the game; regular questions will show up again here.", C_TEXT);
		footer();
		break;
	case SC_FAIL: {
		const char *t = fail.code == KF_NOPIN ? "Game not found" : fail.code == KF_DUPNAME ? "Nickname taken" : fail.code == KF_BADNAME ? "Nickname not allowed"
			: fail.code == KF_LOCKED ? "Game locked" : fail.code == KF_UNSUPPORTED ? "Not supported" : fail.code == KF_KICKED ? "Removed from the game"
			: fail.code == KF_ENDED ? "Game over" : "Connection problem";
		header(t, pin);
		bigMessage(t, fail.text, fail.code == KF_ENDED ? C_TEXT : C_BAD);
		break;
	}
	}
	if (reconnecting && sc != SC_FAIL && sc != SC_HOME) {
		ui_round(40, 70, 320, 90, 12, C_CARD);
		ui_spinner(80, 115, 14, C_ACCENT2);
		ui_text(110, 92, 0.6f, C_TEXT, AL_LEFT, "Connection lost");
		ui_text(110, 118, 0.5f, C_MUTED, AL_LEFT, "Getting back into the game...");
	} else if (paused && sc != SC_FAIL && sc != SC_HOME) {
		ui_round(60, 80, 280, 60, 12, C_CARD);
		ui_text(200, 98, 0.65f, C_TEXT, AL_CENTER, "The host paused the game");
	}

	// ===== bottom screen
	ui_bottom();
	switch (sc) {
	case SC_HOME: {
		ui_text(16, 10, 0.5f, C_MUTED, AL_LEFT, "Game PIN");
		bool p = ui_touchIn(10, 28, 300, 46);
		ui_round(10, 28, 300, 46, 10, p ? C_LINE : C_CARD2);
		ui_text(160, 36, 0.85f, pin[0] ? C_TEXT : C_MUTED, AL_CENTER, pin[0] ? pin : "Tap to enter");
		if (ui_tapped(10, 28, 300, 46)) askText(pin, sizeof pin, "Game PIN", true, 10);
		ui_text(16, 82, 0.5f, C_MUTED, AL_LEFT, "Nickname");
		p = ui_touchIn(10, 100, 300, 46);
		ui_round(10, 100, 300, 46, 10, p ? C_LINE : C_CARD2);
		ui_textFit(160, 110, 0.75f, nick[0] ? C_TEXT : C_MUTED, AL_CENTER, 280, nick[0] ? nick : "Tap to enter");
		if (ui_tapped(10, 100, 300, 46) && askText(nick, sizeof nick, "Nickname", false, 15)) saveSettings();
		bool ready = pin[0] && nick[0];
		if (ready) { if (ui_button(10, 172, 300, 52, NULL, "Join game", true, IC_PLAY) || (k & KEY_A)) startJoin(); }
		else ui_buttonOff(10, 172, 300, 52, "Join game", IC_NONE);
		if (k & KEY_START) quit = true;
		break;
	}
	case SC_CONNECT:
		ui_text(160, 70, 0.55f, C_MUTED, AL_CENTER, "Connecting to kahoot.it");
		if (ui_button(70, 170, 180, 48, NULL, "Cancel", false, IC_NONE) || (k & KEY_B)) leaveGame();
		break;
	case SC_NAMERATOR:
		if (spinsLeft > 0) { snprintf(b, sizeof b, "Spin again (%d left)", spinsLeft); if (ui_button(10, 40, 300, 50, NULL, b, false, IC_NONE)) { makeName(); spinsLeft--; } }
		else ui_buttonOff(10, 40, 300, 50, "No spins left", IC_NONE);
		if (ui_button(10, 110, 300, 56, NULL, "Use this name", true, IC_NONE) || (k & KEY_A)) {
			snprintf(nick, sizeof nick, "%s", genName); kahoot_setName(genName); statusText[0] = 0; go(SC_CONNECT);
		}
		if (ui_button(70, 184, 180, 44, NULL, "Leave", false, IC_NONE) || (k & KEY_B)) leaveGame();
		break;
	case SC_TWOSTEP: {
		static const KEvent four = { .numChoices = 4 };
		KEvent save = q; q = four;
		int hit = twoSent ? -1 : tiles(&four, 0, 190, NULL, NULL, NULL);
		q = save;
		if (hit >= 0 && twoN < 4) {
			twoSeq[twoN++] = hit; twoWrong = false;
			if (twoN == 4) { kahoot_twoStep(twoSeq); twoSent = true; }
		}
		if (ui_button(70, 196, 180, 38, NULL, "Leave", false, IC_NONE) || (k & KEY_B)) leaveGame();
		break;
	}
	case SC_QUESTION: {
		if (isText(q.qtype)) {
			ui_textBox(10, 10, 300, 60, 0.55f, 0.45f, C_MUTED, AL_CENTER, q.qtype[0] == 'w' ? "Type a word or short phrase for the word cloud." : "Type your answer. Spelling counts!");
			if (ui_button(10, 80, 300, 70, NULL, "Type answer", true, IC_NONE) || (k & KEY_A)) {
				if (askText(typed, sizeof typed, "Your answer", false, 20)) { kahoot_answerText(q.index, q.qtype, typed); go(SC_SENT); }
			}
		} else if (isMulti(q.qtype)) {
			int hit = tiles(&q, 0, 192, picked, NULL, NULL);
			if (hit >= 0) picked[hit] = !picked[hit];
			int n = 0, c[K_MAXCHOICES]; for (int i = 0; i < q.numChoices; i++) if (picked[i]) c[n++] = i;
			if (n) { if (ui_button(60, 196, 200, 40, NULL, "Submit", true, IC_NONE) || (k & KEY_X)) { kahoot_answerMulti(q.index, q.qtype, c, n); go(SC_SENT); } }
			else ui_buttonOff(60, 196, 200, 40, "Submit", IC_NONE);
		} else if (isJumble(q.qtype)) {
			int badge[K_MAXCHOICES] = { 0 }; bool dim[K_MAXCHOICES] = { 0 };
			for (int i = 0; i < orderN; i++) { badge[order[i]] = i + 1; }
			int hit = tiles(&q, 0, 192, NULL, badge, dim);
			if (hit >= 0 && !badge[hit] && orderN < q.numChoices) order[orderN++] = hit;
			if (ui_button(8, 196, 100, 40, NULL, "Reset", false, IC_NONE) || (k & KEY_Y)) orderN = 0;
			if (orderN == q.numChoices) { if (ui_button(116, 196, 196, 40, NULL, "Submit", true, IC_NONE) || (k & KEY_X)) { kahoot_answerMulti(q.index, q.qtype, order, orderN); go(SC_SENT); } }
			else ui_buttonOff(116, 196, 196, 40, "Tap in order", IC_NONE);
		} else {
			int hit = tiles(&q, 0, 240, NULL, NULL, NULL);
			if (hit >= 0) sendChoice(hit);
		}
		break;
	}
	case SC_SENT: case SC_TIMEUP: {
		if (chosen >= 0 && q.numChoices) {
			int st = tileStyle(&q, chosen);
			ui_round(60, 40, 200, 130, 14, TILE[st]);
			if (q.hasChoiceText) { shape(st, 82, 62, 12, C_WHITE); ui_textBox(72, 84, 176, 80, 0.6f, 0.4f, C_WHITE, AL_CENTER, q.choice[chosen]); }
			else shape(st, 160, 105, 36, C_WHITE);
		} else if (typed[0] && sc == SC_SENT) {
			ui_round(20, 60, 280, 80, 14, C_CARD2);
			ui_textBox(30, 66, 260, 68, 0.75f, 0.45f, C_TEXT, AL_CENTER, typed);
		} else ui_textBox(20, 60, 280, 80, 0.6f, 0.45f, C_MUTED, AL_CENTER, sc == SC_SENT ? "Answer sent." : "No answer this time.");
		break;
	}
	case SC_NOTYPE:
		if (!strcmp(q.qtype, "content")) ui_textBox(20, 40, 280, 120, 0.65f, 0.45f, C_TEXT, AL_CENTER, "This is a slide. Look at the host's screen; nothing to answer.");
		else { snprintf(b, sizeof b, "This question type (%s) can't be answered on PinPlay. Watch the host's screen; you'll be back for the next question.", typeName(q.qtype, q.layout));
			ui_textBox(20, 40, 280, 140, 0.6f, 0.42f, C_TEXT, AL_CENTER, b); }
		break;
	case SC_FAIL: {
		bool again = fail.code == KF_NET || fail.code == KF_DUPNAME || fail.code == KF_BADNAME;
		if (fail.code == KF_DUPNAME || fail.code == KF_BADNAME) {
			if (ui_button(10, 60, 300, 56, NULL, "Choose another nickname", true, IC_NONE) || (k & KEY_A)) {
				if (askText(nick, sizeof nick, "Nickname", false, 15)) { saveSettings(); startJoin(); }
			}
		} else if (again) { if (ui_button(10, 60, 300, 56, NULL, "Try again", true, IC_NONE) || (k & KEY_A)) startJoin(); }
		if (ui_button(70, 150, 180, 50, NULL, fail.code == KF_ENDED || fail.code == KF_KICKED ? "Done" : "Back", false, IC_NONE) || (k & KEY_B)
			|| (!again && (k & KEY_A))) leaveGame();
		break;
	}
	default: {   // lobby, get ready, results, ranking: a summary card and a Leave button
		ui_round(10, 10, 300, 110, 12, C_CARD);
		ui_textFit(160, 20, 0.7f, C_TEXT, AL_CENTER, 280, nick);
		snprintf(b, sizeof b, "%d points", score); ui_text(160, 54, 0.75f, C_ACCENT2, AL_CENTER, b);
		if (rank > 0) { snprintf(b, sizeof b, "%s place", ordinal(rank, o)); ui_text(160, 88, 0.55f, C_MUTED, AL_CENTER, b); }
		else ui_text(160, 88, 0.5f, C_MUTED, AL_CENTER, sc == SC_LOBBY ? "In the lobby" : "");
		if (confirmLeave) {
			ui_text(160, 132, 0.55f, C_TEXT, AL_CENTER, "Leave this game?");
			if (ui_button(10, 160, 145, 48, NULL, "Stay", false, IC_NONE) || (k & KEY_B)) confirmLeave = false;
			else if (ui_button(165, 160, 145, 48, NULL, "Leave", true, IC_NONE) || (k & KEY_A)) leaveGame();
		} else if (sc == SC_RANKING) {
			if (ui_button(10, 160, 300, 50, NULL, "Back to start", true, IC_NONE) || (k & KEY_A)) leaveGame();
		} else if (ui_button(70, 170, 180, 44, NULL, "Leave game", false, IC_NONE) || (k & KEY_B)) confirmLeave = true;
		break;
	}
	}
	if (sc != SC_HOME && (k & KEY_START)) confirmLeave = true;
	ui_end();
}

// ---- test builds
#ifdef PP_SHOTS
static int fakeStep = -1; static u64 fakeAt;
static void fakeEvents(void) {           // walks through every screen with made-up game data, one screenshot each
	static const char *names[] = { "home", "connect", "lobby", "namerator", "twostep", "getready", "quiz_text", "quiz_shapes", "truefalse", "sent", "result_right",
		"result_wrong", "multi", "jumble", "typeanswer", "poll_result", "notype", "timeup", "ranking", "fail_pin", "fail_dup", "reconnect" };
	if (osGetTime() < fakeAt) return;
	if (fakeStep >= 0) shotNow(names[fakeStep]);
	fakeStep++; fakeAt = osGetTime() + 700;
	if (fakeStep >= (int)(sizeof names / sizeof *names)) { quit = true; return; }
	KEvent e; memset(&e, 0, sizeof e);
	snprintf(pin, sizeof pin, "1234567"); snprintf(nick, sizeof nick, "DuckDude");
	reconnecting = false;
	switch (fakeStep) {
	case 0: go(SC_HOME); break;
	case 1: go(SC_CONNECT); snprintf(statusText, sizeof statusText, "Connecting securely..."); break;
	case 2: e.type = KE_JOINED; snprintf(e.text, sizeof e.text, "DuckDude"); handle(&e); break;
	case 3: e.type = KE_NAMERATOR; handle(&e); break;
	case 4: e.type = KE_TWOSTEP; handle(&e); twoSeq[0] = 2; twoSeq[1] = 0; twoN = 2; break;
	case 5: e.type = KE_GET_READY; e.index = 2; e.total = 10; snprintf(e.qtype, 32, "quiz"); e.timeMs = 5000; handle(&e); break;
	case 6: e.type = KE_QUESTION; e.index = 2; e.total = 10; snprintf(e.qtype, 32, "quiz"); e.timeMs = 20000; e.numChoices = 4; e.hasChoiceText = true;
		snprintf(e.title, sizeof e.title, "Which planet in our solar system has the most moons that astronomers have confirmed so far?");
		snprintf(e.choice[0], 160, "Jupiter"); snprintf(e.choice[1], 160, "Saturn"); snprintf(e.choice[2], 160, "Neptune, the windy blue ice giant far away"); snprintf(e.choice[3], 160, "Mars");
		handle(&e); break;
	case 7: e.type = KE_QUESTION; e.index = 3; e.total = 10; snprintf(e.qtype, 32, "quiz"); e.timeMs = 20000; e.numChoices = 4; handle(&e); sel = 1; break;
	case 8: e.type = KE_QUESTION; e.index = 4; e.total = 10; snprintf(e.qtype, 32, "quiz"); snprintf(e.layout, 24, "TRUE_FALSE"); e.timeMs = 10000; e.numChoices = 2;
		snprintf(e.title, sizeof e.title, "The 3DS came out in 2011."); handle(&e); break;
	case 9: chosen = 1; go(SC_SENT); break;
	case 10: e.type = KE_RESULT; e.hasAnswer = e.correct = true; e.points = 912; e.score = 2711; e.rank = 2; e.streak = 3; snprintf(e.nemesis, 48, "QuizWhiz"); e.nemesisScore = 2950; handle(&e); break;
	case 11: e.type = KE_RESULT; e.hasAnswer = true; e.score = 2711; e.rank = 4; snprintf(e.qtype, 32, "quiz"); handle(&e); break;
	case 12: e.type = KE_QUESTION; e.index = 5; e.total = 10; snprintf(e.qtype, 32, "multiple_select_quiz"); e.timeMs = 30000; e.numChoices = 4; handle(&e); picked[0] = picked[3] = true; break;
	case 13: e.type = KE_QUESTION; e.index = 6; e.total = 10; snprintf(e.qtype, 32, "jumble"); e.timeMs = 30000; e.numChoices = 4; e.hasChoiceText = true;
		snprintf(e.title, sizeof e.title, "Put these in order, smallest first"); snprintf(e.choice[0], 160, "Moon"); snprintf(e.choice[1], 160, "Earth"); snprintf(e.choice[2], 160, "Sun"); snprintf(e.choice[3], 160, "Pebble");
		handle(&e); order[0] = 3; order[1] = 0; orderN = 2; break;
	case 14: e.type = KE_QUESTION; e.index = 7; e.total = 10; snprintf(e.qtype, 32, "open_ended"); e.timeMs = 20000; snprintf(e.title, sizeof e.title, "What is the capital of Norway?"); handle(&e); break;
	case 15: snprintf(q.qtype, 32, "survey"); e.type = KE_RESULT; e.hasAnswer = true; e.score = 3100; e.rank = 2; snprintf(e.qtype, 32, "survey"); handle(&e); break;
	case 16: e.type = KE_QUESTION; e.index = 8; e.total = 10; snprintf(e.qtype, 32, "slider"); handle(&e); break;
	case 17: e.type = KE_QUESTION; e.index = 8; e.total = 10; snprintf(e.qtype, 32, "quiz"); e.numChoices = 4; handle(&e); chosen = -1; go(SC_TIMEUP); break;
	case 18: e.type = KE_RANKING; e.rank = 2; e.score = 7420; e.correctCount = 7; e.incorrectCount = 3; snprintf(e.medal, 16, "silver"); handle(&e); break;
	case 19: e.type = KE_FAIL; e.code = KF_NOPIN; snprintf(e.text, sizeof e.text, "There's no game with that PIN. Check it and try again."); handle(&e); break;
	case 20: e.type = KE_FAIL; e.code = KF_DUPNAME; snprintf(e.text, sizeof e.text, "Someone in this game already has that nickname."); handle(&e); break;
	case 21: go(SC_LOBBY); reconnecting = true; break;
	}
}
#endif

#ifdef PP_AUTOJOIN
static Screen lastShot = (Screen)-1; static u64 shotAt, answerAt; static int answered = -1; static u64 endAt;
static const char *scName(Screen s) {
	static const char *n[] = { "home", "connect", "namerator", "lobby", "twostep", "quizstart", "getready", "question", "sent", "timeup", "result", "ranking", "feedback", "notype", "mode", "fail" };
	return n[s];
}
static void autoDrive(void) {
	if (sc != lastShot) { lastShot = sc; shotAt = osGetTime() + 400; }
	if (shotAt && osGetTime() > shotAt) { shotAt = 0; shotNow(scName(sc)); }
	if (sc == SC_NAMERATOR && osGetTime() - screenStart > 1500) { snprintf(nick, sizeof nick, "%s", genName); kahoot_setName(genName); go(SC_CONNECT); }
	if (sc == SC_QUESTION && answered != questionsSeen) {
		if (!answerAt) answerAt = osGetTime() + 1500;
		if (osGetTime() > answerAt) {
			answerAt = 0; answered = questionsSeen;
			int n = q.numChoices > 0 ? q.numChoices : 4, c = q.index % n;
			plog("auto answer q%d type %s -> %d", q.index, q.qtype, c);
			if (isText(q.qtype)) { snprintf(typed, sizeof typed, "test"); kahoot_answerText(q.index, q.qtype, typed); go(SC_SENT); }
			else if (isMulti(q.qtype)) { kahoot_answerMulti(q.index, q.qtype, &c, 1); go(SC_SENT); }
			else if (isJumble(q.qtype)) { int o4[4] = { 0, 1, 2, 3 }; kahoot_answerMulti(q.index, q.qtype, o4, n < 4 ? n : 4); go(SC_SENT); }
			else sendChoice(c);
		}
	}
	if ((sc == SC_RANKING || sc == SC_FAIL) && !endAt) endAt = osGetTime() + 8000;
	if (endAt && osGetTime() > endAt) quit = true;
}
#endif

int main(void) {
	romfsInit();
	gfxInitDefault();
	log_open();
	plog("PinPlay " VERSION);
	ui_init();
	bool netOk = tls_init();
	if (!netOk) plog("tls_init: %s", tls_initError());
	loadSettings();
#ifdef PP_SOLVETEST
	{   // checks the join-puzzle solver against sdmc:/pinplay_solve.txt (challenge, token, expected session id; repeated)
		FILE *f = fopen("sdmc:/pinplay_solve.txt", "r");
		static char ch[8192], tok[4096], want[2048], got[2048];
		int pass = 0, n = 0;
		while (f && fgets(ch, sizeof ch, f) && fgets(tok, sizeof tok, f) && fgets(want, sizeof want, f)) {
			ch[strcspn(ch, "\n")] = 0; tok[strcspn(tok, "\r\n")] = 0; want[strcspn(want, "\r\n")] = 0;
			bool ok = kahoot_solve(ch, tok, got, sizeof got) && !strcmp(got, want);
			n++; pass += ok;
			plog("solve case %d: %s", n, ok ? "PASS" : "FAIL");
		}
		if (f) fclose(f);
		plog("solvetest %d/%d passed", pass, n);
		quit = true;
	}
#endif
#ifdef PP_AUTOJOIN
	{ FILE *f = fopen("sdmc:/pinplay_test.txt", "r");
	  if (f) { char l1[32] = "", l2[40] = ""; fgets(l1, sizeof l1, f); fgets(l2, sizeof l2, f); fclose(f);
		l1[strcspn(l1, "\r\n ")] = 0; l2[strcspn(l2, "\r\n")] = 0;
		snprintf(pin, sizeof pin, "%s", l1); if (l2[0]) snprintf(nick, sizeof nick, "%s", l2); }
	  if (pin[0] && nick[0]) startJoin(); }
#endif
	if (!netOk) { fail.code = KF_NET; snprintf(fail.text, sizeof fail.text, "The network couldn't start: %s", tls_initError()); go(SC_FAIL); }
	while (aptMainLoop() && !quit) {
		hidScanInput();
		ui_input();
		KEvent e;
		while (kahoot_poll(&e)) handle(&e);
#ifdef PP_SHOTS
		fakeEvents();
#endif
#ifdef PP_AUTOJOIN
		autoDrive();
#endif
		frame();
	}
	kahoot_stop();
	svcSleepThread(200000000);
	ui_exit();
	tls_exit();
	log_close();
	gfxExit();
	romfsExit();
	return 0;
}
