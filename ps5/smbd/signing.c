/* Prospero AutoDrive - prospero-smbd: check of the SMB signing code
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 */

#include "signing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

/* From the patched libsmb2 (lib/smb2-signing.h is not public). */
void smb3_aes_cmac_128(uint8_t key[16], uint8_t *msg, uint64_t msg_len, uint8_t mac[16]);
int smb3_cmac_fast(int enable);

#define BENCH_SIZE (1024 * 1024 + 5)   /* odd length: the padded last block too */

static const char *g_impl = "unchecked";
static long long g_fast_mb_s, g_ref_mb_s;

/* No floating point: printf with %f is not proven on the console. */
static int64_t
now_us(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int
rfc4493_ok(void) {
  /* RFC 4493, section 4: the key, a 64-byte message and the CMAC of its
   * first 0, 16, 40 and 64 bytes. */
  static const uint8_t key[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  static const uint8_t msg[64] = {
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
    0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
    0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
  static const struct {
    size_t len;
    uint8_t mac[16];
  } cases[] = {
    {0, {0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
         0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46}},
    {16, {0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
          0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c}},
    {40, {0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
          0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27}},
    {64, {0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
          0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe}},
  };
  uint8_t k[16], m[64], mac[16];

  for(size_t i=0; i<sizeof cases / sizeof cases[0]; i++) {
    memcpy(k, key, sizeof k);
    memcpy(m, msg, sizeof m);
    smb3_aes_cmac_128(k, m, cases[i].len, mac);
    if(memcmp(mac, cases[i].mac, sizeof mac) != 0) {
      return 0;
    }
  }
  return 1;
}

/* MB/s of one CMAC over buf, repeated until 0.2 s have passed. */
static long long
speed(uint8_t *buf, uint8_t key[16], uint8_t mac[16]) {
  int64_t t0 = now_us(), t;
  int64_t n = 0;

  do {
    smb3_aes_cmac_128(key, buf, BENCH_SIZE, mac);
    n++;
    t = now_us() - t0;
  } while(t < 200000);
  return t > 0 ? (long long)(n * BENCH_SIZE / t) : 0;    /* bytes per us = MB/s */
}

void
signing_check(void) {
  uint8_t key[16], mac_fast[16], mac_ref[16];
  uint8_t *buf = malloc(BENCH_SIZE);
  int fast = smb3_cmac_fast(1);

  for(int i=0; i<16; i++) {
    key[i] = (uint8_t)(i * 37 + 11);
  }
  if(fast && !rfc4493_ok()) {
    smb3_cmac_fast(0);
    fast = 0;
    log_msg("signing: AES-NI gives wrong RFC 4493 results, using the reference code");
  }
  if(buf) {
    for(size_t i=0; i<BENCH_SIZE; i++) {
      buf[i] = (uint8_t)(i * 131 + (i >> 9));
    }
    if(fast) {
      g_fast_mb_s = speed(buf, key, mac_fast);
    }
    smb3_cmac_fast(0);
    g_ref_mb_s = speed(buf, key, mac_ref);
    if(fast) {
      smb3_cmac_fast(1);
      if(memcmp(mac_fast, mac_ref, sizeof mac_fast) != 0) {
        smb3_cmac_fast(0);
        fast = 0;
        log_msg("signing: AES-NI differs from the reference code, using the reference code");
      }
    }
    free(buf);
  }
  if(!rfc4493_ok()) {
    g_impl = "WRONG";
    log_msg("signing: AES-CMAC fails the RFC 4493 test vectors");
    return;
  }
  g_impl = fast ? "aes-ni" : "reference";
  log_msg("signing: AES-CMAC %s, %lld MB/s (reference code %lld MB/s)", g_impl,
          fast ? g_fast_mb_s : g_ref_mb_s, g_ref_mb_s);
}

int
signing_status(char *buf, size_t size) {
  return snprintf(buf, size, "signing_cmac=%s\nsigning_cmac_mb_s=%lld (reference %lld)\n", g_impl,
                  smb3_cmac_fast(-1) ? g_fast_mb_s : g_ref_mb_s, g_ref_mb_s);
}
