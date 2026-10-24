/* Prospero AutoDrive - prospero-smbd: network interfaces and link type
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

/* Text for GET /net: one "if=" line per IPv4 address with what the system
 * says about its type, then "link=<wired|wifi|unknown>" for the interface
 * that carries the connection fd, and how that was decided. The PC uses
 * it to pick how fast it gives up on a missing heartbeat (decision of
 * 24.10.2026). Returns the characters written. */
int netinfo_text(int fd, char *buf, size_t size);
