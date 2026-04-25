#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static void log_with_level(const char *level, const char *fmt, va_list args) {
  fprintf(stderr, "[%s] ", level);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
}

void log_info(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_with_level("INFO", fmt, args);
  va_end(args);
}

void log_error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  log_with_level("ERROR", fmt, args);
  va_end(args);
}
