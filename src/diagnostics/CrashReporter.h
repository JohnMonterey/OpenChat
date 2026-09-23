#pragma once

#include <QString>

// Makes a crash impossible to miss and easy to explain.
//
// When the process dies, a report is written next to a minidump (Windows) and
// OpenChat relaunches itself to show it: what happened in plain words, which
// subsystem and which step it was in (from the BlackBox), the stack, and the
// loaded modules. A death no handler can catch — a fail-fast, a kill, a freeze
// the user gave up on — is explained on the next launch from the black box
// file the dead process left behind.
namespace OpenChat::CrashReporter {

struct Options final {
    // Reports, minidumps and the black box file. defaultDirectory() unless a
    // test says otherwise.
    QString directory;
    // True in the report viewer itself: a crash there must not relaunch
    // another viewer.
    bool viewer = false;
};

// Installs every handler and starts the flight recorder, in memory. Call as
// early as possible in main() — after the organisation and application names
// are set, before QGuiApplication is constructed — so that failing to create
// the application (a missing platform plugin) is reported too.
void install(const Options &options);

// Call once QGuiApplication exists: starts the main thread's heartbeat and
// records a clean exit when the application quits normally.
void attachToApplication();

// The interactive application, as opposed to a preview, a capture or a test:
// explains how the previous session ended (pendingReport()), moves the flight
// recorder into its file so this session can be explained in turn, starts the
// freeze detector, and makes a crash relaunch OpenChat to show its report.
// Kept out of the other modes because they run unattended and in parallel.
void startSession();

[[nodiscard]] QString defaultDirectory();

// A report from an earlier session that the user has not been shown yet.
struct PendingReport final {
    enum class Kind { None, Crash, Freeze, UncleanExit };
    Kind kind = Kind::None;
    QString path;
};
// Found by install(); Kind::None when the last session ended cleanly or its
// report was already shown.
[[nodiscard]] PendingReport pendingReport();
// Records that a report has been shown, so no later launch shows it again.
void markSeen(const QString &reportPath);

// Deliberately fails in one of several ways, so a tester can see exactly what
// each kind of report looks like: segv, abort, throw, qfatal, stack-overflow,
// hang, screen-frame (crashes inside the next captured screen-share frame).
[[nodiscard]] bool isSelfTestKind(const QString &kind);
// Runs it. Every kind but screen-frame ends the process; screen-frame arms the
// capture path and returns.
void runSelfTest(const QString &kind);

} // namespace OpenChat::CrashReporter
