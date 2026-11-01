#!/bin/bash
# Run a command in unixnu-toolchain:v1 with the project directory mounted.
#   container-run.sh <cwd> <cmd...>
set -uo pipefail
PROJECT=${PROJECT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
cwd=$1; shift
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e JOBS=${JOBS:-10} -e UNIXNU_TOOLCHAIN=/opt/unixnu-linux \
  -e XNU_TOOLCHAIN_BIN="/opt/unixnu-linux/CommandLineTools/usr/bin /opt/unixnu-linux/XcodeDefault.xctoolchain/usr/bin" \
  -e PATH=/opt/unixnu-linux/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -v "$PROJECT:$PROJECT" -w "$cwd" unixnu-toolchain:v1 "$@"
