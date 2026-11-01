#!/bin/bash
# Entorno macOS para Q2: Homebrew + Python 3.12 + QEMU + openssl@3 + dtc/mtools.
# Usage: SUDO_PASS=... bash mac-setup.sh
set -euo pipefail

export HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_NO_AUTO_UPDATE=1
export NONINTERACTIVE=1

if [ -n "${SUDO_PASS:-}" ]; then
  printf '%s\n' "$SUDO_PASS" | sudo -S -v
fi

if ! command -v brew >/dev/null 2>&1; then
  echo "== instalando Homebrew"
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

eval "$(/opt/homebrew/bin/brew shellenv)"

echo "== brew install"
brew install python@3.12 qemu openssl@3 dtc mtools cmake ninja unifdef llvm

echo "== versiones"
/opt/homebrew/bin/python3.12 --version
"$(brew --prefix qemu)/bin/qemu-system-aarch64" --version | head -1
"$(brew --prefix openssl@3)/bin/openssl" version
command -v dtc mformat mcopy
echo "== Xcode"
xcode-select -p 2>&1 || true
xcodebuild -version 2>&1 | head -3 || true
xcrun --sdk iphoneos --show-sdk-path 2>&1 || true
echo "== fin"
