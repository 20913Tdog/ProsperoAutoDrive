/* Prospero AutoDrive - prospero-smbd: persistent configuration
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 */

#pragma once

#include <stddef.h>

#define CONFIG_ID_LEN       16  /* hex digits */
#define CONFIG_PASSWORD_LEN 24
#define CONFIG_MAX_PAIRED   16
#define CONFIG_PC_NAME_SIZE 64

/* Characters allowed in a PC name (paired=, PAIR pc=). */
#define CONFIG_PC_NAME_CHARS \
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"

typedef struct config {
  char server_id[CONFIG_ID_LEN + 1];
  char user[32];
  char password[CONFIG_PASSWORD_LEN + 1];
  char name[64];
  int has_skew;                         /* skew_s is a measured value */
  long long skew_s;                     /* console minus PC clock, seconds */
  int npaired;                          /* PCs that got the password */
  char paired[CONFIG_MAX_PAIRED][CONFIG_PC_NAME_SIZE];
} config_t;

enum config_result {
  CONFIG_LOADED,       /* existing config.ini, valid */
  CONFIG_CREATED,      /* there was none: first start */
  CONFIG_REPLACED,     /* it was invalid; kept as config.ini.bad */
  CONFIG_MOVED,        /* taken over from old_dir, kept there as config.ini.moved */
};

/* Loads <dir>/config.ini. Without one it takes over a valid
 * <old_dir>/config.ini (old_dir may be NULL), else it creates one with a new
 * server ID and password. Creates dir and the folders above it if needed.
 * Returns a config_result or -errno. */
int config_load(const char *dir, const char *old_dir, config_t *cfg);

/* Writes cfg back to <dir>/config.ini (atomically). Returns 0 or -errno. */
int config_save(const char *dir, const config_t *cfg);

/* Adds a paired PC unless it is already listed; the oldest entry goes
 * when the list is full. Does not save. */
void config_add_paired(config_t *cfg, const char *pc);
