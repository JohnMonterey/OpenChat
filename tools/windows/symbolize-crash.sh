#!/bin/sh
# Names the OpenChat.exe frames in a Windows crash report.
#
#   tools/windows/symbolize-crash.sh <crash-report.txt> [unstripped OpenChat.exe]
#
# The shipped OpenChat.exe is stripped, so a report can only say
# "OpenChat.exe+0x993856". deploy.sh keeps the unstripped executable of every
# package it builds as build-win/symbols/OpenChat-<link stamp>.exe, keyed by
# the stamp the report lists for OpenChat.exe under "Loaded modules"; this
# looks that copy up and resolves each offset against its symbol table.
# Frames in Qt and other DLLs are already named in the report itself.
#
# Needs the MinGW binutils (x86_64-w64-mingw32-nm); run it through
# tools/windows/rootless-toolchain.sh run when they are only in the overlay.
set -e
report="$1"
exe="$2"
if [ -z "$report" ] || [ ! -r "$report" ]; then
    echo "usage: $0 <crash-report.txt> [unstripped OpenChat.exe]" >&2
    exit 2
fi
source_dir="$(cd "$(dirname "$0")/../.." && pwd)"
nm=x86_64-w64-mingw32-nm

# The report's line for the executable: "  0x... size 0x... stamp 0x6ab3b075  OpenChat.exe"
stamp=$(grep -m1 -E 'stamp 0x[0-9a-f]+ +OpenChat\.exe' "$report" \
    | sed 's/.*stamp 0x\([0-9a-f]*\).*/\1/')
if [ -z "$exe" ]; then
    if [ -n "$stamp" ] && [ -e "$source_dir/build-win/symbols/OpenChat-$stamp.exe" ]; then
        exe="$source_dir/build-win/symbols/OpenChat-$stamp.exe"
    else
        exe="$source_dir/build-win/OpenChat.exe"
        echo "warning: no saved symbols for link stamp ${stamp:-unknown};" \
             "using $exe, which may be a different build" >&2
    fi
fi

# ImageBase from the PE optional header: offset 24 + 24 into the NT headers
# for PE32+. Read straight from the file so no objdump is needed.
pe=$(od -An -tu4 -j60 -N4 "$exe" | tr -d ' ')
image_base=$(od -An -tx8 -j$((pe + 24 + 24)) -N8 "$exe" | tr -d ' ')

symbols=$(mktemp)
trap 'rm -f "$symbols"' EXIT
# Code symbols only, without section names (".text.unlikely"), sorted by address.
"$nm" -C --defined-only "$exe" | grep -E '^[0-9a-f]+ [tT] ' | grep -v ' [tT] \.' \
    | sort > "$symbols"

echo "Symbols from $exe (image base 0x$image_base)"
grep -o 'OpenChat\.exe+0x[0-9a-f]*' "$report" | awk '!seen[$0]++' | while read -r frame; do
    offset=${frame#OpenChat.exe+}
    address=$(printf '%016x' $((0x$image_base + offset)))
    # Compared as strings: both sides are 16 zero-padded hex digits.
    match=$(awk -v a="$address" '($1 "") <= (a "") { line = $0 } END { print line }' "$symbols")
    if [ -z "$match" ]; then
        echo "  $frame  ??"
        continue
    fi
    start=$(printf '%s' "$match" | cut -d' ' -f1)
    name=$(printf '%s' "$match" | cut -d' ' -f3-)
    printf '  %s  %s+0x%x\n' "$frame" "$name" $((0x$address - 0x$start))
done
