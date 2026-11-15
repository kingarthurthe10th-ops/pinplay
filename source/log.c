#include "log.h"
#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>

static FILE *lf; static LightLock lk; static u64 t0;
void log_open(void) {
	LightLock_Init(&lk);
	mkdir("sdmc:/3ds", 0777); mkdir("sdmc:/3ds/pinplay", 0777);
	lf = fopen("sdmc:/3ds/pinplay/log.txt", "w");
	t0 = osGetTime();
}
void log_close(void) { if (lf) fclose(lf); lf = NULL; }
void plog(const char *fmt, ...) {
	if (!lf) return;
	LightLock_Lock(&lk);
	static int lines;
	if (lines++ < 20000) {
		fprintf(lf, "%7.2f ", (osGetTime() - t0) / 1000.0);
		va_list a; va_start(a, fmt); vfprintf(lf, fmt, a); va_end(a);
		fputc('\n', lf); fflush(lf);
	}
	LightLock_Unlock(&lk);
}
