// SPDX-License-Identifier: BSD-2-Clause-Patent
// Parser for the "db-runs" boot argument that the DarwinBoot fork appends (change uefi-runs).
//
//   db-runs=<start>+<bytes>,<start>+<bytes>,...
//
// Hexadecimal without 0x, map order, at most `max` entries. The firmware writes "none" when it found
// no run of at least 64 MiB, and nothing at all when this build did not append the argument; the
// caller distinguishes absent, "none", malformed and a real list.
#ifndef ODIN_RUNS_H
#define ODIN_RUNS_H

#include <stdint.h>

namespace odin_runs {

static inline int digit (char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Strict: no spaces, no empty entries, no trailing comma, no "0x", no sign.
static inline bool hex (const char **at, uint64_t *out)
{
  const char *p = *at;
  uint64_t value = 0;
  unsigned digits = 0;
  for (; *p && *p != '+' && *p != ','; p++) {
    int d = digit (*p);
    if (d < 0 || digits >= 16) return false;
    value = (value << 4) | static_cast<uint64_t> (d);
    digits++;
  }
  if (!digits) return false;
  *at = p;
  *out = value;
  return true;
}

static inline bool parse (const char *text, uint64_t (*out)[2], unsigned max, unsigned *count)
{
  const char *p = text;
  unsigned entries = 0;
  if (!text || !out || !count || !max) return false;
  for (;;) {
    uint64_t start = 0, bytes = 0;
    if (entries >= max) return false;
    if (!hex (&p, &start) || *p != '+') return false;
    p++;
    if (!hex (&p, &bytes) || !bytes) return false;
    if (start > UINT64_MAX - bytes) return false;
    out[entries][0] = start;
    out[entries][1] = bytes;
    entries++;
    if (*p == 0) break;
    if (*p != ',') return false;
    p++;
  }
  *count = entries;
  return true;
}

} // namespace odin_runs

#endif
