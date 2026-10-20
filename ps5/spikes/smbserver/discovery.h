/* Prospero AutoDrive - Spike S7: discovery and liveness over UDP
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

/* UDP port of the spike. The product port is still to be chosen. */
#define DISCOVERY_PORT 4446

/* Binds the UDP socket on all interfaces. Returns the fd or -errno. */
int discovery_open(uint16_t port);

/* Reads one datagram from fd and answers it if it is a valid request.
 * name and smb_port go into the reply. */
void discovery_serve(int fd, const char *name, uint16_t smb_port);

/* Appends the discovery counters to a status report. Returns the length
 * written, like snprintf. */
int discovery_status(char *buf, size_t size);

/* For tests: the reply to one request line, or 0 if it is not one.
 * now_ms is the server's UTC time in milliseconds. */
int discovery_reply(const char *req, size_t len, const char *name,
                    uint16_t smb_port, int64_t now_ms,
                    char *out, size_t size);
