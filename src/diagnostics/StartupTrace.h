#pragma once

#include <QString>

#include <chrono>

// `OpenChat --startup-trace`: every step of starting OpenChat, printed to the
// terminal as it happens with how long it took, and saved to a file to send.
//
// Made for testers whose OpenChat is slow to start on a machine nobody else
// can look at. It answers "slow where?" with numbers: how long Windows took to
// load the program and its DLLs before any of OpenChat's own code ran, how
// long each part of opening the profile took (the keychain, the database, the
// check of every page, the saved encryption state), the window, the first
// frame, the audio check, the proxy lookup, and the relay connection.
//
// Everything here does nothing until enable(), so the calls stay in the code.
// Lines go out through a thread of their own: a terminal that is slow, or
// paused because someone clicked into it, never holds OpenChat up and never
// skews the times it reports. That thread also says when a step has been
// running for a while, so a start that hangs shows where it hangs.
namespace OpenChat::StartupTrace {

// Turns tracing on and prints the header. Call first thing in main().
void enable();
[[nodiscard]] bool enabled();

// Also saves every line, the earlier ones included, to `path`.
void saveTo(const QString &path);

// A moment worth noting, such as "first frame on screen". Any thread.
void mark(const QString &what);
// A fact, printed as it is, such as the size of the profile database.
void note(const QString &what);

// The slowest steps so far, with `why` this is a good moment to look.
void summarize(const QString &why);
// Summarizes (unless already done) and writes out everything still queued.
void finish();

// One step, from construction to destruction. Steps nest.
class Step final
{
public:
    explicit Step(const char *what);
    Step(const char *what, const QString &detail);
    ~Step();

    Step(const Step &) = delete;
    Step &operator=(const Step &) = delete;

    // Added to the line that says the step is done.
    void setResult(const QString &result);

private:
    QString m_what;
    QString m_result;
    std::chrono::steady_clock::time_point m_started;
    int m_id = -1;
};

} // namespace OpenChat::StartupTrace
