#!/bin/sh
# A MinGW cross toolchain plus MinGW Qt6/OpenSSL/Opus, without root.
#
# Installs Arch's mingw-w64-* packages and Martchus' ownstuff mingw-w64-qt6-*
# packages into a private directory using pacman inside a user namespace, and
# then runs commands with that directory overlaid on /usr through bubblewrap,
# so the cross scripts beside this one see everything at the paths they expect
# (/usr/bin/x86_64-w64-mingw32-gcc, /usr/x86_64-w64-mingw32, ...). The host's
# own /usr always wins on a conflict; nothing on the real system is changed.
#
#   tools/windows/rootless-toolchain.sh install   # once, ~2 GB
#   tools/windows/rootless-toolchain.sh run <command...>
#
# Needs: an Arch host with the native qt6-* 6.11 packages (for the host tools),
# pacman, bwrap (bubblewrap) >= 0.10, unprivileged user namespaces and
# unprivileged overlayfs (kernel >= 5.11). OPENCHAT_MINGW_ROOT overrides the
# private root's location.
set -e
root="${OPENCHAT_MINGW_ROOT:-$HOME/.local/share/openchat-mingw}"
packages="mingw-w64-gcc mingw-w64-cmake mingw-w64-pkg-config
    mingw-w64-qt6-base mingw-w64-qt6-declarative mingw-w64-qt6-multimedia
    mingw-w64-qt6-svg mingw-w64-qt6-websockets mingw-w64-qt6-httpserver
    mingw-w64-openssl mingw-w64-opus mingw-w64-wine"

pacman_here() {
    unshare -r pacman --config "$root/etc/pacman.conf" --root "$root" \
        --dbpath "$root/var/lib/pacman" --cachedir "$root/var/cache/pacman/pkg" \
        --noconfirm "$@"
}

case "${1:-}" in
install)
    mkdir -p "$root/etc" "$root/var/lib/pacman" "$root/var/cache/pacman/pkg"
    cat > "$root/etc/pacman.conf" <<'CONF'
[options]
Architecture = x86_64
SigLevel = Never
[ownstuff]
Server = https://martchus.no-ip.biz/repo/arch/ownstuff/os/$arch
[core]
Include = /etc/pacman.d/mirrorlist
[extra]
Include = /etc/pacman.d/mirrorlist
CONF
    pacman_here -Sy
    # Resolve the full dependency closure, then install only the mingw-w64-*
    # members of it: the host-side dependencies (glibc, gcc-libs, zlib, ...)
    # are already on the machine and must not be shadowed.
    # shellcheck disable=SC2086
    mingw_closure=$(pacman_here -Sp --print-format '%n' $packages | grep '^mingw-w64' | sort -u)
    # shellcheck disable=SC2086
    pacman_here -Sdd $mingw_closure
    echo "Installed into $root"
    ;;
run)
    shift
    [ -d "$root/usr" ] || { echo "No toolchain in $root; run '$0 install' first" >&2; exit 1; }
    exec bwrap --dev-bind / / \
        --overlay-src /usr --overlay-src "$root/usr" --tmp-overlay /usr \
        -- "$@"
    ;;
*)
    echo "usage: $0 install | run <command...>" >&2
    exit 2
    ;;
esac
