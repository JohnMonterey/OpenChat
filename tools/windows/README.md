# Windows build from Linux

Cross-compiles `OpenChat.exe` with MinGW-w64, assembles a self-contained
folder next to it, and runs that folder under Wine to prove every DLL it needs
is present and loads. Verified on an Arch host with Wine 11 and Qt 6.11.2.

## Prerequisites

The MinGW sysroot `/usr/x86_64-w64-mingw32` must hold Qt 6 (Base, Declarative,
Multimedia, Svg, WebSockets, HttpServer), OpenSSL 3 and Opus, and the matching
native Qt 6 must be in `/usr` for the host tools (moc, rcc, qmlcachegen,
qmlimportscanner). On Arch that is `mingw-w64-toolchain` from the official
repos plus the `mingw-w64-qt6-*`, `mingw-w64-openssl` and `mingw-w64-opus`
packages from Martchus' [ownstuff](https://martchus.no-ip.biz/repo/arch/ownstuff)
repository.

Without root, `rootless-toolchain.sh install` puts exactly those packages into
`~/.local/share/openchat-mingw` and `rootless-toolchain.sh run <cmd>` overlays
them onto `/usr` for the duration of `<cmd>` (bubblewrap plus unprivileged
overlayfs; the real system is untouched). Prefix every command below with
`tools/windows/rootless-toolchain.sh run` in that case.

The Rust bridge needs the `x86_64-pc-windows-gnu` target:
`rustup target add x86_64-pc-windows-gnu`. Cargo is told to use it
automatically when CMake is cross-compiling with MinGW.

## Steps

```sh
tools/windows/cross-configure.sh build-win          # CMake, Ninja, Release
tools/windows/cross-build.sh build-win OpenChat     # or no target for everything
tools/windows/deploy.sh build-win                   # -> build-win/dist
tools/windows/wine-check.sh build-win/dist          # runs it, no toolchain needed
```

`deploy.sh` copies the Qt plugin categories the app can load, the QML modules
`qmlimportscanner` finds in `qml/`, a `qt.conf` pointing at both, and then
closes the DLL import graph against the sysroot's `bin/` until nothing is
missing. Windows system DLLs are never in the sysroot, so they are skipped.

`wine-check.sh` runs `OpenChat.exe --capture` in a throwaway Wine prefix with
Wine's loader trace on, and prints which deployed modules loaded, any loader
errors, and the size of the capture. Exit status 0 and a real PNG mean the
platform plugin, the QML modules and every imported DLL resolved. Pass
`--call`, `--verify`, `--add-contact` or `--onboarding` to render those
surfaces instead. `OPENCHAT_WINE_HEADLESS=1` uses Qt's offscreen platform;
it goes on the command line because Wine drops all `QT_*` environment
variables before starting a Windows process.

## Pitfalls met along the way

- Do not put `/usr/x86_64-w64-mingw32/bin` on `PATH`: it contains a MinGW `ld`,
  and SQLCipher's configure then cannot build its host-side helper tools.
- SQLCipher's autosetup uses `CC_FOR_BUILD` (default `cc`) for those helpers;
  the scripts export it so the cross `CC` CMake passes is not used for them.
- Wine strips `QT_*` from the environment, so `QT_QPA_PLATFORM` and
  `QT_QUICK_BACKEND` have no effect under Wine; use `-platform`.
