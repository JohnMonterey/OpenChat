#!/bin/sh
# Assemble a self-contained Windows folder from the MinGW cross build: the
# executable, the Qt plugins and QML modules it needs, a qt.conf pointing at
# them, and every DLL reachable through the import tables (Qt, OpenSSL, Opus,
# the GCC runtime), pulled from the MinGW sysroot. Windows system DLLs are
# never present there, so they are skipped naturally.
#
#   tools/windows/deploy.sh [build-dir] [dist-dir]
set -e
build_dir="${1:-build-win}"
dist="${2:-$build_dir/dist}"
sysroot=/usr/x86_64-w64-mingw32
qt_qml="$sysroot/lib/qt6/qml"
qt_plugins="$sysroot/lib/qt6/plugins"
objdump=x86_64-w64-mingw32-objdump
source_dir="$(cd "$(dirname "$0")/../.." && pwd)"

# A build that links can still call into nothing; never package one.
"$source_dir/tools/windows/check-branches.sh" "$build_dir/OpenChat.exe"

rm -rf "$dist"
mkdir -p "$dist/plugins" "$dist/qml"
cp "$build_dir/OpenChat.exe" "$dist/"
# The sysroot's DLLs arrive stripped; the freshly linked executable does not.
x86_64-w64-mingw32-strip "$dist/OpenChat.exe"
# Keep the unstripped executable, named by the link stamp of the stripped one
# (strip rewrites it), which is what every crash report lists for OpenChat.exe.
# tools/windows/symbolize-crash.sh finds it by that stamp to name the frames.
pe_offset=$(od -An -tu4 -j60 -N4 "$dist/OpenChat.exe" | tr -d ' ')
link_stamp=$(od -An -tx4 -j$((pe_offset + 8)) -N4 "$dist/OpenChat.exe" | tr -d ' ')
mkdir -p "$build_dir/symbols"
cp "$build_dir/OpenChat.exe" "$build_dir/symbols/OpenChat-$link_stamp.exe"
echo "Symbols kept as $build_dir/symbols/OpenChat-$link_stamp.exe"

# What a tester needs to know: crash reports and the screen-share check.
cp "$source_dir/tools/windows/tester-readme.txt" "$dist/README.txt"

cat > "$dist/qt.conf" <<'CONF'
[Paths]
Prefix = .
Plugins = plugins
QmlImports = qml
CONF

# Plugin categories a Qt Quick, Multimedia and Network app loads at runtime.
# (No `styles`: those are Qt Widgets styles, and nothing here uses Widgets.)
for category in platforms imageformats iconengines tls multimedia networkinformation; do
    [ -d "$qt_plugins/$category" ] || continue
    mkdir -p "$dist/plugins/$category"
    cp "$qt_plugins/$category"/*.dll "$dist/plugins/$category/"
done

# QML modules: whatever the sources import, transitively, as resolved by the
# host's qmlimportscanner against the MinGW Qt's module tree. Each module
# directory is copied whole, since a style's impl sub-modules are reached from
# compiled-in QML the scanner cannot see.
qmlimportscanner -rootPath "$source_dir/qml" -importPath "$qt_qml" \
    | grep -o '"relativePath": "[^"]*"' | sed 's/"relativePath": "\(.*\)"/\1/' | sort -u \
    | while read -r rel; do
        [ -d "$qt_qml/$rel" ] || continue
        mkdir -p "$dist/qml/$rel"
        cp -r "$qt_qml/$rel"/. "$dist/qml/$rel/"
    done
find "$dist/qml" -type d -name designer -prune -exec rm -rf {} +

# The app imports no QtQuick.Controls itself; the Controls tree arrives only
# through QtQuick.Dialogs' non-native fallback. Keep the styles that fallback
# can actually select on Windows (Basic and the default FluentWinUI3) and drop
# the rest, along with modules the sources never import. This is what keeps
# Qt6Widgets and five style plugins out of the package. Must run before the
# import-graph closure below, which would otherwise pull their DLLs back in.
for unused in \
    QtQuick/Controls/Material QtQuick/Controls/Universal QtQuick/Controls/Imagine \
    QtQuick/Controls/Fusion QtQuick/Controls/Windows \
    QtQuick/Particles QtQuick/LocalStorage QtQuick/tooling Qt/labs; do
    rm -rf "$dist/qml/$unused"
done

# Close the DLL import graph. Names in import tables carry arbitrary case, so
# match case-insensitively against the sysroot's bin directory.
copy_missing_imports() {
    added=0
    for pe in $(find "$dist" -iname '*.exe' -o -iname '*.dll'); do
        for name in $("$objdump" -p "$pe" | awk '/DLL Name:/ {print $3}'); do
            lower=$(printf '%s' "$name" | tr 'A-Z' 'a-z')
            [ -e "$dist/$lower" ] && continue
            src=$(find "$sysroot/bin" -maxdepth 1 -iname "$name" | head -n 1)
            [ -n "$src" ] || continue
            cp "$src" "$dist/$lower"
            added=1
        done
    done
    return $added
}
# Qt's OpenSSL TLS backend opens libssl and libcrypto by name at run time rather
# than importing them, so the import walk below cannot see them. Without them the
# backend fails to initialise and Qt falls back to SChannel; ship them so TLS
# behaves the same as on the other platforms.
if [ -e "$dist/plugins/tls/qopensslbackend.dll" ]; then
    for ssl in "$sysroot"/bin/libssl-3*.dll "$sysroot"/bin/libcrypto-3*.dll; do
        [ -e "$ssl" ] || continue
        cp "$ssl" "$dist/$(basename "$ssl" | tr 'A-Z' 'a-z')"
    done
fi

until copy_missing_imports; do :; done

echo "Deployed to $dist:"
du -sh "$dist" | cut -f1
ls "$dist"/*.dll | xargs -n1 basename | tr '\n' ' '; echo
