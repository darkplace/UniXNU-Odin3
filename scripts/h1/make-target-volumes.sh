#!/bin/bash
# Decode the OS and SystemOS volumes of the IPSW into p2 and p3 ready raw images, on Linux.
#
#   UNIXNU_IPSW=<restore image> UNIXNU_TREE=<UniXNU checkout> bash scripts/h1/make-target-volumes.sh
#
# It runs make-target-volumes.py inside unixnu-toolchain:v1 because the OS image carries LZFSE
# chunks and the decoder for them ships with the toolchain (/opt/unixnu-linux/bin/lzfse). No Apple
# tool is involved and no device is touched.
set -uo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
UNIXNU_TREE=${UNIXNU_TREE:-$(cd "$REPO/../UniXNU" 2>/dev/null && pwd || echo "$REPO/../UniXNU")}
IPSW=${IPSW:-${UNIXNU_IPSW:-$UNIXNU_TREE/../ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw}}
[ -f "$IPSW" ] || { echo "IPSW not found: $IPSW (export UNIXNU_IPSW=...)" >&2; exit 1; }
[ -d "$UNIXNU_TREE" ] || { echo "UNIXNU_TREE not found: $UNIXNU_TREE" >&2; exit 1; }
exec docker run --rm --user "$(id -u):$(id -g)" \
  -e HOME=/tmp -e UNIXNU_IPSW="$IPSW" -e UNIXNU_TREE="$UNIXNU_TREE" \
  -e PATH=/opt/unixnu-linux/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  -v "$(dirname "$REPO"):$(dirname "$REPO")" \
  -v "$UNIXNU_TREE:$UNIXNU_TREE" \
  -v "$(dirname "$IPSW"):$(dirname "$IPSW")" \
  -w "$REPO" unixnu-toolchain:v1 \
  python3 "$REPO/scripts/h1/make-target-volumes.py" --tree "$UNIXNU_TREE" --repo "$REPO" "$@"
