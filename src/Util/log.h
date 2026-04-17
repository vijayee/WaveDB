/**
 * Copyright (c) 2020 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See `log.c` for details.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>

#define LOG_VERSION "0.1.0"

typedef struct {
  va_list ap;
  const char *fmt;
  const char *file;
  struct tm *time;
  void *udata;
  int line;
  int level;
} log_Event;

typedef void (*log_LogFn)(log_Event *ev);
typedef void (*log_LockFn)(bool lock, void *udata);

#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_WARN  3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_FATAL 5

/* Legacy aliases used in log_log() calls */
#define LOG_TRACE  LOG_LEVEL_TRACE
#define LOG_DEBUG  LOG_LEVEL_DEBUG
#define LOG_INFO   LOG_LEVEL_INFO
#define LOG_WARN   LOG_LEVEL_WARN
#define LOG_ERROR  LOG_LEVEL_ERROR
#define LOG_FATAL  LOG_LEVEL_FATAL

#ifndef LOG_COMPILE_LEVEL
#define LOG_COMPILE_LEVEL LOG_LEVEL_INFO
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_TRACE
#define log_trace(...) log_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_trace(...) ((void)0)
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_DEBUG
#define log_debug(...) log_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_debug(...) ((void)0)
#endif

#if LOG_COMPILE_LEVEL <= LOG_LEVEL_INFO
#define log_info(...)  log_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#else
#define log_info(...)  ((void)0)
#endif

/* warn, error, fatal are always compiled in — they're rare and important */
#define log_warn(...)  log_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

const char* log_level_string(int level);
void log_set_lock(log_LockFn fn, void *udata);
void log_set_level(int level);
void log_set_quiet(bool enable);
int log_add_callback(log_LogFn fn, void *udata, int level);
int log_add_fp(FILE *fp, int level);

void log_log(int level, const char *file, int line, const char *fmt, ...);

#endif
