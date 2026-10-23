/* Prospero AutoDrive - prospero-smbd: check of the SMB signing code
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

/* Checks the AES-CMAC of libsmb2 (patch libsmb2-cmac-aesni) against the
 * test vectors of RFC 4493 and against the reference code, and measures
 * both. If the AES-NI path is wrong it is switched off, so signing stays
 * correct. Run once at start, before any connection. */
void signing_check(void);

/* "signing_cmac=..." lines for the status. */
int signing_status(char *buf, size_t size);
