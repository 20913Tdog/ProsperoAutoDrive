/* Prospero AutoDrive - prospero-smbd: HMAC-SHA256
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

/* HMAC-SHA256 (RFC 2104, FIPS 180-4) of msg under key. */
void hmac_sha256(const void *key, size_t key_len, const void *msg,
                 size_t msg_len, uint8_t out[32]);

/* Compares the lowercase hex of mac with hex (64 characters) in constant
 * time. Returns 1 if they are equal. */
int hmac_hex_equal(const uint8_t mac[32], const char *hex, size_t hex_len);
