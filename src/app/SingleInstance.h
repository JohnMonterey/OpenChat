#pragma once

#include <QLockFile>
#include <QObject>
#include <QString>

#include <chrono>

class QLocalServer;

namespace OpenChat {

// One running OpenChat per user.
//
// Starting OpenChat while it is already running used to start a second,
// complete client on the same profile: a second relay link for the same
// device, and a second writer on the same database. When the first one
// happened to be writing at that moment the second could not open the
// profile, and it quit without showing anything — which, from the outside,
// is OpenChat sometimes not launching at all. It is also exactly what a
// slow start invites: nothing on screen yet, so the user starts it again.
//
// Now the first launch holds a lock and answers on a local socket. A later
// launch asks it to bring its window forward and exits. A launch that finds
// the lock held but nobody answering has caught the previous OpenChat on its
// way out, and waits for it to finish before starting.
class SingleInstance final : public QObject
{
    Q_OBJECT

public:
    enum class Claim {
        // This process is the one OpenChat; carry on starting.
        Primary,
        // The running OpenChat was asked to show itself; this process is done.
        HandedOver,
        // Another OpenChat holds the lock, does not answer, and did not exit
        // in time: it is stuck. Nothing can be started alongside it.
        StillRunning,
    };

    // `directory` holds the lock, and names the socket, so one user's
    // OpenChat never answers another's.
    explicit SingleInstance(const QString &directory, QObject *parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance &) = delete;
    SingleInstance &operator=(const SingleInstance &) = delete;

    // Blocks for at most `waitForPrevious` while a previous OpenChat finishes
    // closing.
    [[nodiscard]] Claim claim(std::chrono::milliseconds waitForPrevious);

    // This OpenChat is closing: later launches must wait for the lock rather
    // than hand themselves to a process that is about to be gone.
    void stopAnswering();

    // Lets the next OpenChat start at once (a restart from this one).
    void release();

    [[nodiscard]] QString serverName() const { return m_serverName; }

signals:
    // Another launch asked for this OpenChat's window.
    void activationRequested();

private:
    [[nodiscard]] bool askRunningInstanceToShowItself();
    [[nodiscard]] bool takeOverLeftoverLock();
    void becomePrimary();
    void listen();
    void readRequest();

    QString m_serverName;
    QString m_lockPath;
    QLockFile m_lock;
    QLocalServer *m_server = nullptr;
};

} // namespace OpenChat
