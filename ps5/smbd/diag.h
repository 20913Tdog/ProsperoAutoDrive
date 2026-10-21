/* Prospero AutoDrive - prospero-smbd: queue diagnosis (P2.4)
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

/* Call once per server loop pass: samples every connection. */
void diag_sample(void);

/* Restarts the maxima and averages. */
void diag_reset(void);

/* Appends "diag_..." status lines; returns the characters written. */
int diag_status(char *buf, size_t size);
