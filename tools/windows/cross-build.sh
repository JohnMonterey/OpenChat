#!/bin/sh
# Build (a target of) the MinGW cross build configured by cross-configure.sh.
#   tools/windows/cross-build.sh [build-dir] [ninja args...]
set -e
build_dir="${1:-build-win}"
shift || true
. /usr/bin/mingw-env x86_64-w64-mingw32
export CC_FOR_BUILD="${CC_FOR_BUILD:-cc}"
exec cmake --build "$build_dir" -- "$@"
