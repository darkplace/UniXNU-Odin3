#!/bin/bash
# Host test of the stack probe update rule (drivers/OdinSd/StackFold.h). No device needed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DRV=$ROOT/devices/odin3-pagani/drivers/OdinSd
OUT=${OUT:-$(mktemp -d)}
${CXX:-g++} -std=c++17 -O1 -Wall -I"$DRV" -o "$OUT/stack_fold" "$DRV/test/stack_fold.cpp"
"$OUT/stack_fold"
