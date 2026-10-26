/* Prospero AutoDrive - prospero-smbd: monotonic clock
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Time since an arbitrary start that does not move when someone sets the
 * console's clock. For timers and durations only; log lines, now_utc, the
 * clock skew and file times stay on the wall clock. CLOCK_MONOTONIC is
 * proven on the console (name cache, signing check).
 */

#pragma once

#include <stdint.h>
#include <time.h>

/* Seconds, never 0, so 0 can mean "not set". */
static inline time_t
mono_s(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1;
}

static inline int64_t
mono_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
