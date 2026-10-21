/* Prospero AutoDrive - prospero-smbd: ring log
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 */

#pragma once

/* Logs to <dir>/log.txt. When it reaches LOG_MAX_BYTES it becomes
 * log.1.txt, replacing the older one, and a new log.txt starts. Returns 0
 * or -errno; without a log every log_msg() only goes to stdout. */
int log_open(const char *dir);

/* One line "<UTC time> <message>", also to stdout. */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void log_close(void);

#define LOG_MAX_BYTES (1024 * 1024)
