#ifndef PD2HOLYGRAIL_LOG_H
#define PD2HOLYGRAIL_LOG_H

/* A file log is the only way to see anything from inside the game process: the game owns the
   screen, and under CrossOver there is no console to print to. Everything this plugin does is
   written here, next to the DLL, so a failed restart can be read after the fact instead of
   guessed at. */
void log_init(void *module);
void log_line(const char *fmt, ...);

#include <windows.h>
/* The DLL's folder, with its trailing backslash ("" before log_init has run). */
const char *log_dir(void);
/* When log_init ran — the start of this game session. */
const SYSTEMTIME *log_session_start(void);

#endif
