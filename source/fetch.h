#pragma once
// Background HTTPS downloads (pictures for pin questions, the avatar list) on their own thread,
// so the game connection keeps answering while they run.
#include <3ds.h>
#include <stdbool.h>

enum { FETCH_RUNNING, FETCH_DONE, FETCH_FAILED };

typedef struct Fetch Fetch;
// url: https://host/path. decodePng: also turn a PNG reply into RGBA pixels (fetch_pixels).
Fetch *fetch_start(const char *url, bool decodePng);
int fetch_state(Fetch *f);
const char *fetch_body(Fetch *f, int *len);           // the reply (NUL-terminated), when FETCH_DONE
const u32 *fetch_pixels(Fetch *f, int *w, int *h);    // RGBA8 as 0xRRGGBBAA, when decodePng and FETCH_DONE
void fetch_free(Fetch *f);                            // safe while running (the thread tidies up)
