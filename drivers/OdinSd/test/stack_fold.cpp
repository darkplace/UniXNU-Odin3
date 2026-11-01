// Host test of the stack-margin update rule (StackFold.h). The first version of the probe never recorded a positive
// margin; ODIN_SD_TEST turns sdStackMark into a no-op, so nothing in the driver model could have noticed.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "StackFold.h"

// The rule that shipped in sd-stack-probe (SdHost.h:58 at 4f29ee2), kept here to show what the test would have caught.
static int64_t shippedRule (int64_t slot, int64_t remaining) { return remaining < slot ? remaining : slot; }

int main ()
{
  // A slot never sampled takes the first margin, whatever it is.
  assert (sdStackFold (SD_STACK_UNREACHED, 0x3958) == 0x3958);
  assert (sdStackFold (SD_STACK_UNREACHED, 0) == 0);
  assert (sdStackFold (SD_STACK_UNREACHED, -0x60) == -0x60);
  // Afterwards only a smaller margin wins.
  assert (sdStackFold (0x3958, 0x4000) == 0x3958);
  assert (sdStackFold (0x3958, 0x0400) == 0x0400);
  assert (sdStackFold (0x0400, -0x10) == -0x10);
  assert (sdStackFold (-0x10, 0x3000) == -0x10);
  // A margin that was really measured as -1 is not mistaken for "unreached".
  assert (sdStackFold (-1, 0x100) == -1);
  // The shipped rule starting from -1 (as SdHost.h:45 initialised it) cannot record any margin >= 0.
  int64_t slot = -1;
  for (int64_t margin : { 0x3958, 0x2a10, 0x0400, 0 }) slot = shippedRule (slot, margin);
  assert (slot == -1);
  // Replay of the 4 samples with the corrected rule.
  int64_t fixed = SD_STACK_UNREACHED;
  for (int64_t margin : { 0x3958, 0x2a10, 0x0400, 0x1000 }) fixed = sdStackFold (fixed, margin);
  assert (fixed == 0x0400);
  puts ("PASS: stack fold");
  return 0;
}
