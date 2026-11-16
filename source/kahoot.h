#pragma once
// Joins a live Kahoot game as one player, the way the kahoot.it web page does:
// reserve the session, solve the join puzzle, then talk CometD/Bayeux over a secure websocket.
// Everything runs on a network thread; the UI reads events with kahoot_poll().
#include <3ds.h>
#include <stdbool.h>

#define K_MAXCHOICES 6

typedef enum {
	KE_STATUS,          // text: what it's doing now
	KE_FAIL,            // code + text: the session is over
	KE_NAMERATOR,       // the host makes players use generated names: call kahoot_setName()
	KE_JOINED,          // in the lobby (text = nickname)
	KE_TWOSTEP,         // two-step join: tap the 4 shapes in the order shown on the host's screen
	KE_TWOSTEP_WRONG, KE_TWOSTEP_OK,
	KE_QUIZ_START,
	KE_GET_READY,       // index, total, qtype, timeMs
	KE_QUESTION,        // the full question
	KE_TIME_UP,
	KE_RESULT,          // right/wrong, points, score, rank
	KE_RANKING,         // end of the game: final rank and score
	KE_FEEDBACK,        // host asks players to rate the kahoot (kahoot_sendFeedback)
	KE_PLAY_AGAIN,      // back to the lobby for another round
	KE_PAUSE, KE_RESUME,
	KE_TEAM_TALK,
	KE_RECONNECTING, KE_RECONNECTED,
	KE_MODE_UNSUPPORTED,// the host started a special game mode PinPlay can't play
	KE_LOBBY,           // after a reconnect: still waiting for the game to start
	KE_WAIT_NEXT,       // joined between questions: waiting for the next one
	KE_AVATAR,          // the host allows avatars: canChange, avatarType/avatarItem = current, text = publishedAt
	KE_AVATAR_OFF,      // avatars turned off
} KEvType;

enum { KF_NONE, KF_NOPIN, KF_NET, KF_DUPNAME, KF_BADNAME, KF_LOCKED, KF_UNSUPPORTED, KF_KICKED, KF_ENDED, KF_CANCEL };

typedef struct {
	KEvType type;
	int code;
	char text[256];
	// question / get ready
	int index, total, numChoices, timeMs, allowed;
	char qtype[32], layout[24];
	char title[480];
	char choice[K_MAXCHOICES][160];
	bool hasChoiceText;
	bool answered;      // (rejoining) this question was already answered
	// slider: min..max in steps; scale/nps: start..end with end labels
	double sMin, sMax, sStep; char unit[32];
	int scStart, scEnd; char scLow[64], scHigh[64], scLabelType[16];
	char image[512];    // picture URL (pin questions)
	// avatar
	bool canChange; int avatarType, avatarItem;
	// result / ranking
	bool hasAnswer, correct;
	int points, score, rank, streak, correctCount, incorrectCount;
	char nemesis[48]; int nemesisScore;
	char answerText[160];
	char medal[16];
} KEvent;

void kahoot_start(const char *pin, const char *nickname);   // starts a new session (stops any old one)
void kahoot_stop(void);                                       // leave the game
bool kahoot_poll(KEvent *ev);                                 // next event, false if none
void kahoot_setName(const char *name);                        // answer to KE_NAMERATOR
void kahoot_answerChoice(int qIndex, const char *qtype, int choice);
void kahoot_answerMulti(int qIndex, const char *qtype, const int *choices, int n);
void kahoot_answerText(int qIndex, const char *qtype, const char *text);
void kahoot_answerNumber(int qIndex, const char *qtype, double value, int decimals);   // slider, scale, nps
void kahoot_answerPin(int qIndex, const char *qtype, double x, double y);              // drop_pin / pin_it, 0..100 % of the picture
void kahoot_twoStep(const int seq[4]);
void kahoot_setAvatar(int type, int item);
void kahoot_sendFeedback(int fun, int learning, int recommend, int overall, int totalScore);   // stars 1-5, thumbs 0/1, face -1/0/1
const char *kahoot_nickname(void);

// pieces exposed for the self-test
bool kahoot_solve(const char *challenge, const char *tokenB64, char *out, int outLen);
