#!/bin/sh
# Diablo II 1.13c is a 32-bit process, so this is i686 and not negotiable.
# Needs mingw-w64 (brew install mingw-w64).
set -e
cd "$(dirname "$0")"
${CC:-i686-w64-mingw32-gcc} -shared -o pd2dpsmeter.dll src/*.c \
  -Os -std=c99 -Wall -Wextra -static-libgcc \
  -Wl,--enable-stdcall-fixup
echo "built pd2dpsmeter.dll"
