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
	KE_FEEDBACK,        // host asks for feedback (not offered here; just a waiting screen)
	KE_PLAY_AGAIN,      // back to the lobby for another round
	KE_PAUSE, KE_RESUME,
	KE_TEAM_TALK,
	KE_RECONNECTING, KE_RECONNECTED,
	KE_MODE_UNSUPPORTED,// the host started a special game mode PinPlay can't play
	KE_LOBBY,           // after a reconnect: still waiting for the game to start
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
void kahoot_twoStep(const int seq[4]);
const char *kahoot_nickname(void);

// pieces exposed for the self-test
bool kahoot_solve(const char *challenge, const char *tokenB64, char *out, int outLen);
