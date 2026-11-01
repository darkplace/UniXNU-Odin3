#!/usr/bin/env python3
"""Host test of the db-runs parser of the Odin 3 SD driver (change uefi-runs).

Compiles devices/odin3-pagani/drivers/OdinSd/OdinRuns.h against a small main and checks the cases
that matter: what the DarwinBoot fork writes, what it must reject, and the eight-entry cap.
No device, no card.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "devices/odin3-pagani/drivers/OdinSd"

FIXTURE = r"""
#include <cstdio>
#include <cstring>
#include "OdinRuns.h"

static int failures = 0;
static void expect (const char *text, bool ok, unsigned count, uint64_t first, uint64_t bytes)
{
  uint64_t out[8][2] = {};
  unsigned got = 0;
  bool parsed = odin_runs::parse (text, out, 8, &got);
  bool good = parsed == ok && (!ok || (got == count && out[0][0] == first && out[0][1] == bytes));
  if (!good) {
    std::printf ("  FAIL %-42s parsed=%d got=%u\n", text ? text : "(null)", parsed, got);
    failures++;
  }
}

int main ()
{
  expect ("8c0000000+340000000", true, 1, 0x8c0000000ULL, 0x340000000ULL);
  expect (" 8c0000000+340000000", false, 0, 0, 0);            // leading space is not in the grammar
  expect ("8c0000000+340000000,80000000+80000000", true, 2, 0x8c0000000ULL, 0x340000000ULL);
  expect ("8C0000000+340000000", true, 1, 0x8c0000000ULL, 0x340000000ULL);
  expect ("none", false, 0, 0, 0);
  expect ("", false, 0, 0, 0);
  expect ("8c0000000", false, 0, 0, 0);
  expect ("8c0000000+0", false, 0, 0, 0);
  expect ("8c0000000+", false, 0, 0, 0);
  expect ("+340000000", false, 0, 0, 0);
  expect ("8c0000000+340000000,", false, 0, 0, 0);
  expect ("8c0000000+340000000,,1+1", false, 0, 0, 0);
  expect ("0x8c0000000+1", false, 0, 0, 0);
  expect ("ffffffffffffffff+1", false, 0, 0, 0);
  expect ("fffffffffffffff0+f", true, 1, 0xfffffffffffffff0ULL, 0xfULL);
  expect ("10000000000000000+1", false, 0, 0, 0);              // 17 hex digits
  expect ("1+1,2+2,3+3,4+4,5+5,6+6,7+7,8+8", true, 8, 1, 1);
  expect ("1+1,2+2,3+3,4+4,5+5,6+6,7+7,8+8,9+9", false, 0, 0, 0);
  expect ("zz+1", false, 0, 0, 0);
  if (failures == 0) std::printf ("PASS: db-runs parser\n");
  return failures != 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="odin-runs-") as temporary:
        source = Path(temporary) / "runs.cpp"
        source.write_text(FIXTURE)
        binary = Path(temporary) / "runs"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HEADER),
                        str(source), "-o", str(binary)], check=True)
        return subprocess.run([str(binary)]).returncode


if __name__ == "__main__":
    sys.exit(main())
