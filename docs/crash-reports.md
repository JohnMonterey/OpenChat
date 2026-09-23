# Crash reports

When OpenChat crashes, the tester should never be left guessing. The crash
handler writes a report, then OpenChat restarts itself showing a window that
says what went wrong, in plain words, and what OpenChat was doing. The report
is one click away from the clipboard. A death that no handler can catch is
explained on the next launch instead.

## What a report says

The report is a text file that starts with a summary the window shows as it is:

```
OpenChat crash report
=====================

What happened:  The program tried to read memory at 0x10, which is not valid: a null pointer.
Where:          nvwgf2umx.dll+0x1a2b3c
Thread:         main (id 4312)
Doing:          screen share: copying the desktop image on the GPU
Hint:           The crash happened inside the NVIDIA graphics driver. Updating the graphics driver usually fixes this.
```

Below the summary the report has these sections:

- **Application, build, system, start and crash times.**
- **What OpenChat was set up to do.** A line per active subsystem, for example
  `screen share: Windows Desktop Duplication, \\.\DISPLAY1, 2560x1440, on NVIDIA
  GeForce RTX 3070` and `call: Active one-to-one call, camera on`.
- **Threads.** What each described thread was doing, with the crashed one
  marked.
- **Recent events.** The last 128, oldest first: what the user chose, which
  capture API opened, fallbacks taken, errors, and Qt warnings (repeats are
  collapsed).
- **Stack.** On Windows it is unwound from the CPU context with the modules'
  own unwind tables, and frames in DLLs are named after the nearest export. On
  Linux and macOS it comes from `backtrace()`.
- **Registers and loaded modules, with link timestamps** (Windows), or the
  executable mappings (Linux).

On Windows a **minidump** (`.dmp`) is written beside it.

For a deliberate stop the report says why. Examples are `OpenChat stopped
itself on purpose: Qt fatal error: …` and `… an exception nobody caught: …`.
**Where** then points at the code that asked for the stop, not at `abort()`.
Crashes inside graphics drivers, Direct3D, capture services, and overlays that
other programs inject (Steam, Discord, RivaTuner, OBS) get a **Hint**.

Reports never contain message text, names, handles or window titles, because
they are meant to be sent.

## Where reports are kept

| Platform | Folder |
|---|---|
| Windows | `%LOCALAPPDATA%\OpenChat\OpenChat\crashes` |
| macOS | `~/Library/Application Support/OpenChat/OpenChat/crashes` |
| Linux | `~/.local/share/OpenChat/OpenChat/crashes` |

The newest 20 reports are kept, with their minidumps. File names are in UTC:

- `crash-…txt` is written by the crash handler.
- `freeze-…txt` is written while the window is frozen.
- `frozen-…txt` and `closed-…txt` are written on the next launch for a session
  that died without a handler running.

## How it works

`src/diagnostics/`:

- **BlackBox**: the flight recorder. It holds fixed-size slots for recent
  events, per-subsystem context, and what each thread is doing. The slots are
  written lock-free, so a handler can read them without allocating.
  `BlackBox::Activity` is a scope cheap enough to wrap every captured frame.
  In an interactive session the recorder lives in a memory-mapped file
  (`crashes/blackbox.bin`). A shared mapping's pages belong to the operating
  system, so even a kill, a fail-fast or a power-off leaves the last events on
  disk.
- **CrashHandlerWin**: installs a structured-exception filter, a first-chance
  handler for stack overflow, the CRT's abort, pure-call and invalid-parameter
  hooks, and turns off the callback exception swallowing that 64-bit Windows
  applies to window procedures. The filter only wakes a watcher thread started
  at install and waits for it. That thread has a healthy stack, writes the
  report and minidump, and relaunches `OpenChat.exe --crash-report <path>`. A
  Qt fatal error is raised as our own exception before Qt's
  `RaiseFailFastException` can bypass every handler.
- **CrashHandlerPosix**: installs signal handlers on an alternate stack. The
  report is written with raw `write()`. The viewer is relaunched with
  `posix_spawn`, not `fork`, because `fork` takes the allocator's locks and a
  crash inside `malloc` would hang there. Then the signal is re-raised so the
  system still writes its core file or macOS crash report. A deliberate `SIGTERM`, `SIGHUP` or `SIGINT`
  counts as a clean exit.
- **CrashWriter**: the allocation-free formatter both handlers share, including
  a hand-rolled UTC calendar (`gmtime` is not signal-safe).
- **CrashReporter**: installs everything before `QGuiApplication` exists, so a
  missing platform plugin is reported too. It also hooks Qt's message handler
  and `std::terminate`.
  - A freeze detector watches the main thread's heartbeat. After 10 s of
    silence it writes a freeze report with the main thread's stack. On Windows
    it suspends the thread just long enough to read its registers; on POSIX it
    signals the thread to record its own return addresses. A suspended machine and an attached debugger are not treated as
    freezes.
  - On the next launch it reads the previous session's black box and writes
    the `frozen-`/`closed-` report.

Only the interactive application keeps the recorder on disk, detects freezes
and relaunches after a crash. The `--capture` and preview modes run unattended
and several at a time, so a crash there writes a report and nothing else.

## Known limits

- **Stack overflow on a thread other than the main one (Linux/macOS).** Only
  the main thread has an alternate signal stack, so the kernel ends the process
  with no report. On the next launch the black box still explains it as
  "closed unexpectedly", including what that thread was last doing.
- **Loader or heap lock held by the crash.** The Windows watcher still calls
  `CreateFileW`, `CreateProcessW` and `MessageBoxW`. A crash inside the loader
  or the process heap can hold the lock those calls need. The filter then gives
  up after a minute. Everything written before that point is already on disk,
  in order of value: the summary and black box, the "a report exists" marker,
  the minidump, and only then the stack walk.
- **Fail-fast exits.** Windows heap corruption and `__fastfail` bypass every
  handler, as a `SIGKILL` does. These are what the next-launch `closed-` report
  is for.
- **Two instances.** A second OpenChat started while one is running keeps its
  recorder in memory. `crashes/session.lock` decides which instance owns the
  file, so neither mistakes the other for a dead session.

## Self-tests for testers

`OpenChat --crash-test <kind>` fails on purpose once the window is up, so
anyone can see exactly what each kind of report looks like:

- `segv`
- `abort`
- `throw`
- `qfatal`
- `stack-overflow`
- `pure-virtual`: a pure virtual function called through a real vtable. On
  Linux and macOS OpenChat's own `__cxa_pure_virtual` names it. The MinGW
  build never links one: GCC declares it weak, and the MinGW linker resolves
  weak symbols unreliably (a definition of our own was bound to an unrelated
  function). So on Windows the call jumps to address 0. The report explains
  that jump, and names the caller under "Where".
- `hang`: freezes the main thread for 45 s. Kill it to see the next-launch
  report.
- `screen-frame`: crashes inside the next captured screen-share frame. Combine
  it with `--screen-share-check` to try it without a call.

`OPENCHAT_CRASH_UNATTENDED=1` keeps the reports but puts nothing on screen,
which is how the automated tests use it.

## Reading a report as a developer

- **Windows.** `deploy.sh` keeps the unstripped executable of every package as
  `build-win/symbols/OpenChat-<link stamp>.exe`. Run
  `tools/windows/symbolize-crash.sh <report>`: it finds that copy by the stamp
  the report lists for `OpenChat.exe` and names each `OpenChat.exe+0x…` frame.
  The minidump opens in WinDbg or Visual Studio. Microsoft's and the GPU
  vendors' symbol servers name the system and driver frames.
- **Linux.** Run `addr2line -f -C -e OpenChat <offset>` on the
  `OpenChat(+0x…)` frames.
- **macOS.** Use `atos -o OpenChat -l <load address> <address>`, or read the
  system's own `.ips` report under `~/Library/Logs/DiagnosticReports`, which
  the re-raised signal still produces.

Tests:

- `tst_crashreport`: the calendar, the ring, contexts, nested activities, the
  thread table, UTF-8 cutting, hints. On Linux it also runs the real binary
  crashing, and killed mid-freeze then relaunched.
- `tst_qmlload::aCrashReportWindowSaysWhatHappenedAndWhatOpenChatWasDoing`: the
  window.

The Windows handler was exercised under Wine for every self-test kind.
