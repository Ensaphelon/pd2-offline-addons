#ifndef PD2RESTART_LOG_H
#define PD2RESTART_LOG_H

/* A file log is the only way to see anything from inside the game process: the game owns the
   screen, and under CrossOver there is no console to print to. Everything this plugin does is
   written here, next to the DLL, so a failed restart can be read after the fact instead of
   guessed at. */
void log_init(void *module);
void log_line(const char *fmt, ...);

#endif
