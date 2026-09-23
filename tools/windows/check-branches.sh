#!/bin/sh
# Refuses a Windows executable containing a direct call or jump to an address
# outside its own image, or straight to __cxa_pure_virtual.
#
#   tools/windows/check-branches.sh <unstripped OpenChat.exe>
#
# A direct branch always lands inside the program; calls into DLLs go through
# the import table, which is an indirect call. So a direct branch outside the
# image can only be a call to a symbol the linker left at address 0: a weak
# symbol that nothing defined. That is exactly what shipped once. GCC compiled
# a COM call through an interface declared in an anonymous namespace as a
# direct call to __cxa_pure_virtual, which is weak and undefined in this
# build. Sharing a window then jumped 1 GiB below the program and crashed on
# the first click. Nothing else notices, because the code links and runs
# until that line is reached.
#
# deploy.sh runs this before packaging. Needs the MinGW binutils; run it
# through tools/windows/rootless-toolchain.sh run when they are only in the
# overlay.
set -e
exe="$1"
if [ -z "$exe" ] || [ ! -r "$exe" ]; then
    echo "usage: $0 <unstripped OpenChat.exe>" >&2
    exit 2
fi
objdump=x86_64-w64-mingw32-objdump
base=$("$objdump" -p "$exe" | awk '/^ImageBase/ {print $2; exit}')
size=$("$objdump" -p "$exe" | awk '/^SizeOfImage/ {print $2; exit}')
if [ -z "$base" ] || [ -z "$size" ]; then
    echo "check-branches: could not read the image layout of $exe" >&2
    exit 2
fi

# Where the program's own __cxa_pure_virtual is, if it has one. Compared by
# address: the disassembly may label that address with any of the weak aliases
# the compiler left pointing at it.
pure=$(x86_64-w64-mingw32-nm "$exe" | awk '$2 ~ /^[Tt]$/ && $3 ~ /^(\.weak\.)?__cxa_pure_virtual(\.|$)/ && $3 !~ /\.cold$/ {print $1; exit}')

bad=$("$objdump" -d --no-show-raw-insn -C "$exe" | awk -v base="0x$base" -v size="0x$size" -v pure="0x${pure:-0}" '
    BEGIN { low = strtonum(base); high = low + strtonum(size); pure = strtonum(pure) }
    /^[0-9a-f]+ <.*>:$/ { function_name = substr($0, index($0, "<")); next }
    /\t(call|jmp) +[0-9a-f]+ </ {
        split($0, columns, "\t")
        split(columns[2], operands, " +")
        target = strtonum("0x" operands[2])
        # A direct call to __cxa_pure_virtual, should a build ever link one,
        # is the same mistake: compiled, but it can only stop the program.
        if (target < low || target >= high || (pure != 0 && target == pure))
            printf "  %s\n    %s\n", function_name, $0
    }')

if [ -n "$bad" ]; then
    echo "check-branches: $exe has direct branches that can only crash:" >&2
    echo "$bad" >&2
    echo "A direct call to __cxa_pure_virtual is the usual cause: a virtual call through a" >&2
    echo "class GCC believes has no implementation, such as a COM interface declared in an" >&2
    echo "anonymous namespace. Give the class external linkage." >&2
    exit 1
fi
echo "check-branches: every direct branch in $exe stays inside it"
