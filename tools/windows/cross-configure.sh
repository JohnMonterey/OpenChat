#!/bin/sh
# Configure a MinGW cross build of OpenChat from Linux.
#
# Expects the mingw-w64 toolchain, the mingw Qt6 modules, OpenSSL and Opus to
# be installed under /usr/x86_64-w64-mingw32 (Arch's mingw-w64-* packages, or
# Martchus' ownstuff repository for the Qt/OpenSSL/Opus parts) and the matching
# native Qt6 to be installed in /usr for the host tools (moc, rcc, qmlcachegen).
#
#   tools/windows/cross-configure.sh [build-dir] [extra cmake args...]
set -e
build_dir="${1:-build-win}"
shift || true
. /usr/bin/mingw-env x86_64-w64-mingw32
# SQLCipher's autosetup configure builds its own helper tools (jimsh, lemon)
# and must use the host compiler for them, not the cross compiler CMake hands it.
export CC_FOR_BUILD="${CC_FOR_BUILD:-cc}"
exec cmake -S "$(dirname "$0")/../.." -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-x86_64-w64-mingw32.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING_EMULATOR=/usr/bin/x86_64-w64-mingw32-wine \
    -DCMAKE_INSTALL_PREFIX=/usr/x86_64-w64-mingw32 \
    "$@"
