/* Prospero AutoDrive - Spike S2: read-only file system backend
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

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

/* Request handlers for smb2_server.handlers. */
extern struct smb2_server_request_handlers fsro_handlers;

/* Shares the directory root read-only as share_name for one user.
 * Returns 0 or -errno. */
int fsro_init(const char *root, const char *share_name,
              const char *user, const char *password);

/* Call for every accepted connection, before it is served. */
void fsro_attach(struct smb2_context *smb2);

/* Call once per server loop pass: sends replies that had to wait until the
 * library was done with their request. */
void fsro_flush(void);

/* Appends counters and the requests the backend did not support, as
 * "key=value" lines. Returns the number of characters written. */
int fsro_status(char *buf, size_t size);
