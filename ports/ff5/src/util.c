/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

#define LOG_NAME "debug.log"

/* Gated so the fake-JNI/shim trace does not flood the log and disturb preload
   timing on the device. Enable on demand via FF5_VERBOSE / FF5_JNILOG. */
int debugPrintf(const char *text, ...) {
  va_list list;
  static int enabled = -1;

  if (enabled < 0)
    enabled = getenv("FF5_VERBOSE") || getenv("FF5_JNILOG");
  if (!enabled)
    return 0;

  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);

  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
