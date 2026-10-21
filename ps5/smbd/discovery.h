/* Prospero AutoDrive - prospero-smbd: discovery over UDP
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

#define DISCOVERY_PORT 4445   /* UDP; TCP 4445 is the service channel */

typedef struct discovery_info {
  const char *server_id;
  const char *name;
  uint16_t service_port;
  uint16_t smb_port;
} discovery_info_t;

/* Binds the UDP port. Returns the socket or -errno. */
int discovery_open(uint16_t port);

/* Answers one waiting datagram, if it is a valid request. */
void discovery_serve(int fd, const discovery_info_t *info);

/* Builds the reply to one request; returns its length, or 0 if the
 * request is not valid. */
int discovery_reply(const char *req, size_t len, const discovery_info_t *info,
                    char *out, size_t size);

/* Appends "discovery_..." status lines; returns the characters written. */
int discovery_status(char *buf, size_t size);
