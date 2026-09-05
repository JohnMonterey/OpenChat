#!/bin/sh
# Run the deployed Windows build under Wine and prove every DLL it needs is
# found: Wine's loader traces each module it loads, and the app's own --capture
# mode drives the whole QML surface to a rendered PNG and exits, so a zero exit
# plus a real image means the Qt platform plugin, the QML modules and every
# transitively imported DLL all loaded.
#
#   tools/windows/wine-check.sh [dist-dir] [extra OpenChat args...]
#
# Writes wine-check.log (the full loader trace) and capture.png in the dist
# directory. Set WINEPREFIX to reuse a prefix; the default is a throwaway one
# beside the dist directory so the user's own prefix is never touched.
#
# By default the real Windows platform plugin (qwindows) is used, which needs a
# display: that is the path a Windows user actually runs. OPENCHAT_WINE_HEADLESS=1
# selects Qt's offscreen platform instead. It has to go on the command line:
# Wine drops every QT_* variable from the environment it hands to Windows
# processes, so QT_QPA_PLATFORM would silently never arrive.
set -e
dist="${1:-build-win/dist}"
shift || true
dist="$(cd "$dist" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$dist.wineprefix}"
# No Mono or Gecko install prompts: they would block an unattended run.
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEDEBUG="${WINEDEBUG:-+loaddll}"

if [ ! -d "$WINEPREFIX" ]; then
    echo "Creating Wine prefix $WINEPREFIX"
    wineboot -u >/dev/null 2>&1 || true
    wineserver -w
fi

platform_args=""
[ "${OPENCHAT_WINE_HEADLESS:-0}" = 1 ] && platform_args="-platform offscreen"

cd "$dist"
rm -f capture.png wine-check.log
set +e
timeout 120 wine OpenChat.exe $platform_args "$@" \
    --capture capture.png --capture-delay 800 --width 860 --height 680 \
    > wine-check.log 2>&1
status=$?
set -e
wineserver -w

echo "OpenChat.exe exit status: $status"
if [ -s capture.png ]; then
    echo "capture.png: $(wc -c < capture.png) bytes"
else
    echo "capture.png: missing"
fi

echo "--- modules loaded from the deployed folder:"
grep -o 'Loaded L"[^"]*"' wine-check.log | sed 's/Loaded L"//;s/"$//' \
    | grep -i 'OpenChat' | sed 's|.*\\||' | sort -fu | tr '\n' ' '; echo
echo "--- loader errors (missing modules, failed imports):"
grep -iE 'err:module|not found|failed to load|could not load|cannot find' wine-check.log || echo "none"
echo "--- Qt warnings:"
grep -vE 'loaddll|^wine:|^[0-9a-f]+:(fixme|err|warn):(winediag|ole|hid|d3d|dxgi|win|x11drv|ntdll|combase|setupapi|kerberos|hnetcfg|xrandr|rawinput|winstrap|imm|font)' wine-check.log | grep -v '^$' | head -40 || true
exit $status
