/* Prospero AutoDrive - prospero-smbd: persistent configuration
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * config.ini holds "key=value" lines; '#' starts a comment line. Unknown
 * keys are ignored, so later versions can add some. It is written to
 * config.ini.tmp first and then renamed, so a crash leaves either the old
 * or the new file, never half of one. Deleting it resets the server: new
 * ID, new password, pairing open again (phase 2 design).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "config.h"

#define DEFAULT_USER "prospero"
#define DEFAULT_NAME "PS5"
#define MAX_SIZE     4096

/* Letters and digits only: safe in a "net use" command line and in the
 * Credential Manager, 62^24 is about 143 bits. */
static const char alphabet[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";


/* Copies the value if it is 1 to size-1 characters long and only has
 * characters from set (NULL: any printable ASCII). */
static int
take(char *dst, size_t size, const char *v, size_t len, const char *set) {
  if(len < 1 || len >= size) {
    return -1;
  }
  for(size_t i=0; i<len; i++) {
    if(!v[i] || (set ? !strchr(set, v[i]) : (v[i] < 0x20 || v[i] > 0x7e))) {
      return -1;
    }
  }
  memcpy(dst, v, len);
  dst[len] = '\0';
  return 0;
}


static int
parse(const char *text, size_t len, config_t *cfg) {
  const char *p = text;
  const char *end = text + len;
  int bad = 0;

  memset(cfg, 0, sizeof *cfg);
  while(p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    const char *le = eol ? eol : end;
    const char *eq;

    if(le > p && le[-1] == '\r') {
      le--;
    }
    if(le > p && *p != '#' && (eq=memchr(p, '=', (size_t)(le - p)))) {
      size_t klen = (size_t)(eq - p);
      const char *v = eq + 1;
      size_t vlen = (size_t)(le - v);

      if(klen == 9 && !memcmp(p, "server_id", 9)) {
        bad |= vlen != CONFIG_ID_LEN ||
               take(cfg->server_id, sizeof cfg->server_id, v, vlen, "0123456789abcdef");
      } else if(klen == 4 && !memcmp(p, "user", 4)) {
        bad |= take(cfg->user, sizeof cfg->user, v, vlen, "abcdefghijklmnopqrstuvwxyz0123456789_-");
      } else if(klen == 8 && !memcmp(p, "password", 8)) {
        bad |= vlen != CONFIG_PASSWORD_LEN ||
               take(cfg->password, sizeof cfg->password, v, vlen, alphabet);
      } else if(klen == 4 && !memcmp(p, "name", 4)) {
        bad |= take(cfg->name, sizeof cfg->name, v, vlen, CONFIG_PC_NAME_CHARS);
      } else if(klen == 6 && !memcmp(p, "skew_s", 6)) {
        /* By hand: strtoll() is not proven on the console yet. */
        size_t i = vlen && v[0] == '-';
        long long x = 0;

        cfg->has_skew = vlen > i && vlen - i <= 9;
        for(; cfg->has_skew && i < vlen; i++) {
          cfg->has_skew = v[i] >= '0' && v[i] <= '9';
          x = x * 10 + (v[i] - '0');
        }
        cfg->skew_s = v[0] == '-' ? -x : x;
        bad |= !cfg->has_skew;
      } else if(klen == 6 && !memcmp(p, "paired", 6)) {
        char pc[CONFIG_PC_NAME_SIZE];

        if(take(pc, sizeof pc, v, vlen, CONFIG_PC_NAME_CHARS) == 0) {
          config_add_paired(cfg, pc);
        } else {
          bad = 1;
        }
      }
    }
    p = eol ? eol + 1 : end;
  }
  if(bad || !cfg->server_id[0] || !cfg->user[0] || !cfg->password[0]) {
    return -EINVAL;
  }
  if(!cfg->name[0]) {
    snprintf(cfg->name, sizeof cfg->name, "%s", DEFAULT_NAME);
  }
  return 0;
}


static void
generate(config_t *cfg) {
  static const char hex[] = "0123456789abcdef";
  uint8_t raw[CONFIG_ID_LEN / 2];

  memset(cfg, 0, sizeof *cfg);
  arc4random_buf(raw, sizeof raw);
  for(size_t i=0; i<sizeof raw; i++) {
    cfg->server_id[2*i] = hex[raw[i] >> 4];
    cfg->server_id[2*i+1] = hex[raw[i] & 15];
  }
  for(int i=0; i<CONFIG_PASSWORD_LEN; i++) {
    cfg->password[i] = alphabet[arc4random_uniform(sizeof alphabet - 1)];
  }
  snprintf(cfg->user, sizeof cfg->user, "%s", DEFAULT_USER);
  snprintf(cfg->name, sizeof cfg->name, "%s", DEFAULT_NAME);
}


void
config_add_paired(config_t *cfg, const char *pc) {
  for(int i=0; i<cfg->npaired; i++) {
    if(!strcmp(cfg->paired[i], pc)) {
      return;
    }
  }
  if(cfg->npaired == CONFIG_MAX_PAIRED) {
    memmove(cfg->paired[0], cfg->paired[1], sizeof cfg->paired[0] * (CONFIG_MAX_PAIRED - 1));
    cfg->npaired--;
  }
  snprintf(cfg->paired[cfg->npaired++], sizeof cfg->paired[0], "%s", pc);
}


static int
save(const char *path, const char *tmp, const config_t *cfg) {
  char text[MAX_SIZE];
  int n;
  int fd;

  n = snprintf(text, sizeof text,
               "# Prospero AutoDrive, prospero-smbd. Delete this file to reset:\n"
               "# new server ID and password, pairing open again.\n"
               "server_id=%s\nuser=%s\npassword=%s\nname=%s\n",
               cfg->server_id, cfg->user, cfg->password, cfg->name);
  if(cfg->has_skew && n > 0 && n < (int)sizeof text) {
    n += snprintf(text + n, sizeof text - n, "skew_s=%lld\n", cfg->skew_s);
  }
  for(int i=0; i<cfg->npaired && n > 0 && n < (int)sizeof text; i++) {
    n += snprintf(text + n, sizeof text - n, "paired=%s\n", cfg->paired[i]);
  }
  if(n < 0 || n >= (int)sizeof text) {
    return -EINVAL;
  }
  if((fd=open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0) {
    return -errno;
  }
  errno = 0;
  if(write(fd, text, (size_t)n) != n || fsync(fd) != 0) {
    int err = errno ? errno : EIO;
    close(fd);
    unlink(tmp);
    return -err;
  }
  close(fd);
  if(rename(tmp, path) != 0) {
    int err = errno;
    unlink(tmp);
    return -err;
  }
  return 0;
}


static int
paths(const char *dir, char *path, char *tmp, char *bad) {
  if(snprintf(path, 1024, "%s/config.ini", dir) >= 1024) {
    return -ENAMETOOLONG;
  }
  snprintf(tmp, 1040, "%s.tmp", path);
  snprintf(bad, 1040, "%s.bad", path);
  return 0;
}


int
config_save(const char *dir, const config_t *cfg) {
  char path[1024];
  char tmp[1040];
  char bad[1040];
  int rc;

  if((rc=paths(dir, path, tmp, bad))) {
    return rc;
  }
  return save(path, tmp, cfg);
}


int
config_load(const char *dir, config_t *cfg) {
  char path[1024];
  char tmp[1040];
  char bad[1040];
  char text[MAX_SIZE];
  ssize_t len;
  int result = CONFIG_CREATED;
  int rc;
  int fd;

  if((rc=paths(dir, path, tmp, bad))) {
    return rc;
  }
  if(mkdir(dir, 0700) != 0 && errno != EEXIST) {
    return -errno;
  }

  if((fd=open(path, O_RDONLY)) >= 0) {
    len = read(fd, text, sizeof text);
    close(fd);
    if(len >= 0 && len < (ssize_t)sizeof text && parse(text, (size_t)len, cfg) == 0) {
      return CONFIG_LOADED;
    }
    /* Keep the broken one for a look, start over. */
    if(rename(path, bad) != 0) {
      return -errno;
    }
    result = CONFIG_REPLACED;
  } else if(errno != ENOENT) {
    return -errno;
  }

  generate(cfg);
  if((rc=save(path, tmp, cfg))) {
    return rc;
  }
  return result;
}
