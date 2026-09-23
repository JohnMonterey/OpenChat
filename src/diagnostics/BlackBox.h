#pragma once

#include <QString>

// The flight recorder every crash report is built from.
//
// "Access violation at 0x7ff6a1b2c3d4" tells nobody anything. What makes a
// crash obvious is knowing what the application was doing when it happened:
// which subsystem, which step, on which thread, and what led up to it. That is
// what this records — cheaply enough to leave on in every build, and in memory
// a crash handler can read without allocating, locking or calling into Qt.
//
// Once CrashReporter is installed the recorder lives in a memory-mapped file.
// A shared mapping's pages belong to the operating system rather than to the
// process, so even a death no handler can catch — a fail-fast, a kill from Task
// Manager, a freeze the user gave up on — leaves the last events on disk, and
// the next launch explains them.
//
// Nothing recorded here may identify the user or what they are looking at: no
// message text, no names, no window titles. Reports are meant to be shared.
namespace OpenChat::BlackBox {

// One line in the ring of recent events. `area` is a short literal such as
// "screen share".
void record(const char *area, const QString &text);
void record(const char *area, const char *text);

// A standing fact about a subsystem, printed near the top of a report for as
// long as it is set: "screen share" -> "Windows Desktop Duplication, display 1,
// 2560x1440". An empty value clears it.
void setContext(const char *area, const QString &value);

// Names the calling thread in reports ("main", "screen capture").
void nameThread(const char *name);

// The main thread's sign of life, for the hang detector. Called from a timer
// on the main thread.
void heartbeat();

// What the calling thread is doing right now, for exactly as long as the scope
// lives. Scopes nest: the innermost is what a report shows, and the enclosing
// one comes back when it ends. Both strings must outlive the scope — in
// practice they are literals — and only their text is copied, which is what
// makes this cheap enough to wrap every captured frame in.
class Activity final
{
public:
    Activity(const char *area, const char *activity) noexcept;
    ~Activity();

    Activity(const Activity &) = delete;
    Activity &operator=(const Activity &) = delete;

private:
    const char *m_previousArea;
    const char *m_previousActivity;
};

} // namespace OpenChat::BlackBox
