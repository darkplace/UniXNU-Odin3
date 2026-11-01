// SPDX-License-Identifier: BSD-2-Clause-Patent
// StackFold.h - the update rule of the stack margin probe (change mount-panic, fixes sd-stack-probe).
// Kept in its own header so the host test can run the exact rule the kernel build uses.
//
// A slot starts at kSdStackUnreached. The first sample always lands; after that the smallest margin wins. A negative
// margin is real data: ml_stack_remaining() is unsigned (SP - thread->kernel_stack), so an SP inside the guard page
// shows up as -(distance below the stack base) once cast to int64_t.
//
// The first version initialised the slots to -1 and updated with `remaining < slot`: no margin >= 0 is smaller than
// -1, so the slots never moved (measured on the 104afcc3 kernel: cmp x0,x9 / b.ge skip / str x0,[x8,#0xb28]).
#ifndef ODIN_SD_STACK_FOLD_H
#define ODIN_SD_STACK_FOLD_H
#include <stdint.h>

#define SD_STACK_UNREACHED INT64_MAX

static inline int64_t sdStackFold (int64_t slot, int64_t remaining)
{
  return (slot == SD_STACK_UNREACHED || remaining < slot) ? remaining : slot;
}

#endif
