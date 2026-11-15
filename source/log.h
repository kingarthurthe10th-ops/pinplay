#pragma once
// A small log on the SD card (sdmc:/3ds/pinplay/log.txt, started fresh each launch) for finding problems on real hardware.
void log_open(void);
void log_close(void);
void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
