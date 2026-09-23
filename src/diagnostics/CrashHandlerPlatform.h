#pragma once

#include <cstdint>

// The operating-system half of CrashReporter: installing the handlers and
// writing a report from inside them. Implemented once for Windows (structured
// exceptions, a watcher thread, minidumps) and once for POSIX (signals on an
// alternate stack). Private to src/diagnostics.
namespace OpenChat::CrashHandlerPlatform {

struct Config final {
    // UTF-8, without a trailing separator.
    char directory[1024];
    // Relaunch this executable into the report viewer after a crash.
    bool relaunch;
    // Installed by the report viewer: no relaunch.
    bool viewer;
    // Somebody is watching: a crash may put up a message box when there is no
    // viewer to relaunch. Unattended runs only write the report.
    bool interactive;
};

// Installs the fatal handlers. Config is copied; nothing is allocated later.
void install(const Config &config);

// Installs them again. Some third-party code (overlays, drivers) replaces the
// process-wide handler as it loads; this is called once the application is up
// to take it back.
void reinstall();

// Writes a report about the main thread being frozen, including its stack
// where the platform can take one without the thread's help. Called from the
// freeze detector's own thread. Returns false if nothing could be written;
// `path` receives the report's UTF-8 path.
bool writeFreezeReport(std::int64_t frozenSinceMs, char *path, int pathCapacity);

// A Qt fatal error has been recorded and is about to end the process.
// Windows raises the crash here (Qt would otherwise fail-fast past every
// handler); POSIX lets Qt's abort() arrive as SIGABRT.
void fatalMessage();

// The interactive session has started: from now on a crash relaunches the
// viewer (if `relaunch`) and may interrupt the user.
void setInteractive(bool relaunch);

// Called on the main thread at install, so the freeze report can find it.
void rememberMainThread();

// True while a debugger is attached: a thread stopped at a breakpoint is not
// frozen.
bool debuggerAttached();

} // namespace OpenChat::CrashHandlerPlatform
