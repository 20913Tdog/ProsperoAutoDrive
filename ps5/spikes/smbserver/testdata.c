/* Prospero AutoDrive - Spike S2: deterministic test content
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Layout of the new test folder:
 *   hello.txt                        short ASCII text
 *   empty.bin                        0 bytes
 *   MixedCase.txt                    for case-insensitive lookups
 *   pattern-1MiB.bin                 xorshift32 stream, first 1 MiB
 *   pattern-64MiB.bin                same stream, 64 MiB (many READs)
 *   pattern-<N>MiB.bin               same stream, N MiB, only if asked (S4)
 *   Ordner mit Leerzeichen/Grüße äöü €.txt
 *   tief/1/2/3/4/5/6/7/8/ende.txt    deep path
 *   viele/datei-NNNN-....txt         1500 files (directory paging)
 *
 * Expected SHA-256 values: tests/spikes/s2-expected.sha256.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "testdata.h"

#define MANY_FILES   1500
#define PATTERN_SEED 0x50533553u
#define CHUNK        (1u << 20)


static int
join(char *out, size_t size, const char *dir, const char *name) {
  int n = snprintf(out, size, "%s/%s", dir, name);

  return (n < 0 || (size_t)n >= size) ? -ENAMETOOLONG : 0;
}


static int
write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;

  while(len > 0) {
    ssize_t n = write(fd, p, len);

    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      return -errno;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}


/* O_EXCL: a test never overwrites an existing file. */
static int
open_new(const char *dir, const char *name) {
  char path[1024];
  int rc;

  if((rc=join(path, sizeof path, dir, name))) {
    return rc;
  }
  rc = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
  return rc < 0 ? -errno : rc;
}


static int
finish(int fd, int rc) {
  if(close(fd) != 0 && rc == 0) {
    rc = -errno;
  }
  return rc;
}


static int
put_file(const char *dir, const char *name, const char *text) {
  int fd = open_new(dir, name);

  if(fd < 0) {
    return fd;
  }
  return finish(fd, write_all(fd, text, strlen(text)));
}


static int
put_dir(const char *dir, const char *name, char *out, size_t size) {
  int rc;

  if((rc=join(out, size, dir, name))) {
    return rc;
  }
  return mkdir(out, 0755) != 0 ? -errno : 0;
}


/* xorshift32, emitted as little-endian 32-bit words. */
static void
pattern_fill(uint32_t *state, uint8_t *buf, size_t len) {
  uint32_t x = *state;

  for(size_t i=0; i+4 <= len; i+=4) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    buf[i]   = (uint8_t)x;
    buf[i+1] = (uint8_t)(x >> 8);
    buf[i+2] = (uint8_t)(x >> 16);
    buf[i+3] = (uint8_t)(x >> 24);
  }
  *state = x;
}


static int
put_pattern(const char *dir, const char *name, size_t mib) {
  uint32_t state = PATTERN_SEED;
  uint8_t *buf;
  int rc = 0;
  int fd;

  if(!(buf=malloc(CHUNK))) {
    return -ENOMEM;
  }
  if((fd=open_new(dir, name)) < 0) {
    free(buf);
    return fd;
  }
  for(size_t i=0; i<mib && rc == 0; i++) {
    pattern_fill(&state, buf, CHUNK);
    rc = write_all(fd, buf, CHUNK);
  }
  free(buf);
  return finish(fd, rc);
}


static int
put_many(const char *dir) {
  char sub[1024];
  char name[64];
  char text[16];
  int rc = put_dir(dir, "viele", sub, sizeof sub);

  for(int i=1; i<=MANY_FILES && rc == 0; i++) {
    snprintf(name, sizeof name, "datei-%04d-abcdefghijklmnopqrstuvwxyz.txt", i);
    snprintf(text, sizeof text, "%d\r\n", i);
    rc = put_file(sub, name, text);
  }
  return rc;
}


static int
put_deep(const char *dir) {
  char path[1024];
  char next[1024];
  char name[12];
  int rc = put_dir(dir, "tief", path, sizeof path);

  for(int i=1; i<=8 && rc == 0; i++) {
    snprintf(name, sizeof name, "%d", i);
    rc = put_dir(path, name, next, sizeof next);
    memcpy(path, next, sizeof path);
  }
  return rc ? rc : put_file(path, "ende.txt", "tief unten\r\n");
}


int
testdata_create(const char *base, char *out, size_t size, size_t big_mib) {
  time_t now = time(NULL);
  char sub[1024];
  char big[32];
  uint32_t rnd;
  struct tm tm;
  int rc;
  int n;

  arc4random_buf(&rnd, sizeof rnd);
  gmtime_r(&now, &tm);
  n = snprintf(out, size,
               "%s/_ProsperoAutoDrive_Test_%04d%02d%02d-%02d%02d%02d-%06x",
               base, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned)(rnd & 0xffffff));
  if(n < 0 || (size_t)n >= size) {
    return -ENAMETOOLONG;
  }
  if(mkdir(out, 0755) != 0) {
    return -errno;
  }

  if((rc=put_file(out, "hello.txt", "Prospero AutoDrive S2\r\n")) ||
     (rc=put_file(out, "empty.bin", "")) ||
     (rc=put_file(out, "MixedCase.txt", "Gross und klein\r\n")) ||
     (rc=put_pattern(out, "pattern-1MiB.bin", 1)) ||
     (rc=put_pattern(out, "pattern-64MiB.bin", 64)) ||
     (rc=put_dir(out, "Ordner mit Leerzeichen", sub, sizeof sub)) ||
     (rc=put_file(sub, "Grüße äöü €.txt",
                  "Unicode OK\r\n")) ||
     (rc=put_deep(out)) ||
     (rc=put_many(out))) {
    return rc;
  }
  if(big_mib) {
    snprintf(big, sizeof big, "pattern-%uMiB.bin", (unsigned)big_mib);
    return put_pattern(out, big, big_mib);
  }
  return 0;
}
