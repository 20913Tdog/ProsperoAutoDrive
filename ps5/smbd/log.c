/* Prospero AutoDrive - prospero-smbd: ring log
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * The file is written with open()/write()/rename() only; of stdio, only
 * printf() to stdout is proven on the console. The time is the console's
 * UTC, unadjusted.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "log.h"

static struct {
  int fd;
  long size;
  char path[1024];
  char old_path[1024];
} g = {-1, 0, "", ""};


static int
reopen(void) {
  struct stat st;

  if((g.fd=open(g.path, O_WRONLY | O_CREAT | O_APPEND, 0600)) < 0) {
    return -errno;
  }
  g.size = fstat(g.fd, &st) == 0 ? (long)st.st_size : 0;
  return 0;
}


int
log_open(const char *dir) {
  if(snprintf(g.path, sizeof g.path, "%s/log.txt", dir) >= (int)sizeof g.path ||
     snprintf(g.old_path, sizeof g.old_path, "%s/log.1.txt", dir) >= (int)sizeof g.old_path) {
    return -ENAMETOOLONG;
  }
  return reopen();
}


void
log_msg(const char *fmt, ...) {
  char line[1200];
  time_t now = time(NULL);
  struct tm tm;
  va_list args;
  int n;

  memset(&tm, 0, sizeof tm);
  gmtime_r(&now, &tm);
  n = (int)strftime(line, sizeof line, "%Y-%m-%dT%H:%M:%SZ ", &tm);
  va_start(args, fmt);
  vsnprintf(line + n, sizeof line - n - 1, fmt, args);
  va_end(args);
  n = (int)strlen(line);
  line[n++] = '\n';

  printf("%.*s", n, line);
  fflush(stdout);

  if(g.fd < 0) {
    return;
  }
  if(g.size + n > LOG_MAX_BYTES) {
    close(g.fd);
    g.fd = -1;
    rename(g.path, g.old_path);
    if(reopen() != 0) {
      return;
    }
  }
  if(write(g.fd, line, (size_t)n) == n) {
    g.size += n;
  }
}


void
log_close(void) {
  if(g.fd >= 0) {
    close(g.fd);
    g.fd = -1;
  }
}
