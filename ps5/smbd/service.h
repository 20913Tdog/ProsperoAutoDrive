/* Prospero AutoDrive - prospero-smbd: service channel on TCP 4445
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
#include <stdint.h>

#include <sys/select.h>

#define SERVICE_PORT 4445

typedef struct service_info {
  const char *server_id;
  const char *name;
  const char *share;
  uint16_t smb_port;
  /* Appends "key=value" status lines, returns the characters written. */
  int (*status)(char *buf, size_t size);
} service_info_t;

enum service_result {
  SERVICE_OK,
  SERVICE_QUIT,        /* QUIT from this console: end the server */
  SERVICE_BROKEN,      /* the listening socket failed: reopen everything */
};

/* Listens on port. info must stay valid. Returns 0 or -errno. */
int service_open(uint16_t port, const service_info_t *info);

/* Hooks for every pass of the server loop. */
void service_fdset(fd_set *rfds, int *maxfd);
int service_run(const fd_set *rfds);

/* Closes the listener and every connection. */
void service_close(void);

/* Asks a running instance on this console to quit, so that it frees the
 * ports. Works for this server and for the spike payloads. Returns 1 if
 * something answered. */
int service_stop_previous(uint16_t port);
