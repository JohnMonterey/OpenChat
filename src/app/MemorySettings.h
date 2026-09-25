#pragma once

#include <QObject>
#include <QTimer>

namespace OpenChat {

// Settings → General → Low memory mode: the smallest footprint OpenChat can
// manage, paid for in processor time and a few conveniences.
//
// Two parts of it can only be chosen before the first window exists, so they
// follow the setting as it was when the process started
// (lowMemoryModeAtStartup) and a change waits for a restart: drawing without
// the graphics card, and leaving Qt Multimedia unloaded until a call or the
// audio settings need it. The rest follows the switch at once: profile
// pictures are kept compressed and decoded only to be drawn, the chat keeps
// fewer recently shown attachments, and the heap is handed back to the system
// every minute.
class MemorySettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool lowMemoryMode READ lowMemoryMode WRITE setLowMemoryMode NOTIFY
                   lowMemoryModeChanged)
    // True while the saved choice differs from the one this process started
    // with, i.e. while a restart is owed.
    Q_PROPERTY(bool restartPending READ restartPending NOTIFY lowMemoryModeChanged)
public:
    explicit MemorySettings(QObject *parent = nullptr);
    ~MemorySettings() override;

    // The instance main() made for the application, shared with QML.
    [[nodiscard]] static MemorySettings *instance();

    // The saved choice, read the first time it is asked for and fixed for the
    // life of the process. Needs the application's name to be set already.
    [[nodiscard]] static bool lowMemoryModeAtStartup();

    // Set by restartApplication(); main() starts a new process once this one
    // has shut down.
    [[nodiscard]] static bool restartRequested();

    // Hands back to the system the memory the heap is merely holding on to:
    // glibc keeps what short-lived allocations freed for reuse, and an idle
    // chat window rarely reuses it. A no-op where the C library is not glibc.
    static void releaseFreedHeap();

    [[nodiscard]] bool lowMemoryMode() const { return m_lowMemoryMode; }
    void setLowMemoryMode(bool enabled);
    [[nodiscard]] bool restartPending() const;

    // Quits; main() then starts OpenChat again, once the profile is locked and
    // the relay link closed, so the two processes never overlap.
    Q_INVOKABLE void restartApplication();

signals:
    void lowMemoryModeChanged();

private:
    void apply();

    bool m_lowMemoryMode = false;
    QTimer m_trimTimer;
};

} // namespace OpenChat
