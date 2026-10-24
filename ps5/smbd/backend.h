/* Prospero AutoDrive - prospero-smbd: file system backend
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

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

/* Request handlers for smb2_server.handlers. */
extern struct smb2_server_request_handlers backend_handlers;

/* Shares the directory root as share_name for one user, read-write if
 * writable is set, otherwise read-only. profile is the socket/dialect
 * profile new connections start with (see backend_set_profile()).
 * Returns 0 or -errno. */
int backend_init(const char *root, const char *share_name,
                 const char *user, const char *password,
                 int writable, char profile);

/* Seconds the console clock runs ahead of the PC: subtracted from every
 * time reported to the client, added to every time the client sets. */
void backend_set_clock_skew(int64_t seconds);

/* Send buffer of every connection, now and for new ones; 0 returns to
 * the profile's size (P2.4 measurement). */
void backend_set_sndbuf(int bytes);

/* Call for every accepted connection, before it is served. */
void backend_attach(struct smb2_context *smb2);

/* Call with the listening socket once per server loop pass. */
void backend_listener(int fd);

/* Speed diagnosis (S2b): selects socket/dialect profile 'a', 'b' or 'c' for
 * new connections and ends the running ones. Returns 0 or -1. */
int backend_set_profile(char name);

/* Restarts the timing counters shown by backend_status(). */
void backend_reset_stats(void);

/* Call once per server loop pass: sends replies that had to wait until the
 * library was done with their request. */
void backend_flush(void);

/* Adds the backend's own descriptor (end of a write in its thread) to the
 * server loop's select(). */
void backend_fdset(fd_set *rfds, int *maxfd);

/* Appends counters and the requests the backend did not support, as
 * "key=value" lines. Returns the number of characters written. */
int backend_status(char *buf, size_t size);
