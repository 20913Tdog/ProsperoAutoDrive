/* Prospero AutoDrive - Spike S2: deterministic test content
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

/* Creates <base>/_ProsperoAutoDrive_Test_<UTC timestamp>-<random> and fills
 * it with the S2 test files. The folder path is written to out. Never
 * touches anything outside the new folder, never deletes, never retries.
 * Returns 0, or -errno of the first step that failed; out then still names
 * the folder for a read-only look. */
int testdata_create(const char *base, char *out, size_t size);
