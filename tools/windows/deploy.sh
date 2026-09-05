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

rm -rf "$dist"
mkdir -p "$dist/plugins" "$dist/qml"
cp "$build_dir/OpenChat.exe" "$dist/"

cat > "$dist/qt.conf" <<'CONF'
[Paths]
Prefix = .
Plugins = plugins
QmlImports = qml
CONF

# Plugin categories a Qt Quick, Multimedia and Network app loads at runtime.
for category in platforms imageformats iconengines tls multimedia networkinformation generic styles; do
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
until copy_missing_imports; do :; done

echo "Deployed to $dist:"
du -sh "$dist" | cut -f1
ls "$dist"/*.dll | xargs -n1 basename | tr '\n' ' '; echo
