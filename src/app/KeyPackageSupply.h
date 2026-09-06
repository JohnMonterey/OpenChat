#pragma once

#include <QObject>
#include <QTimer>

namespace OpenChat {
class ProfileSession;
class RelayClient;

// Owned by DeviceLink, after bootstrap. All MLS mutations and persistence run
// synchronously on the profile thread; only the HTTP publishes are asynchronous.
class KeyPackageSupply final : public QObject
{
    Q_OBJECT
public:
    static constexpr int minimumAvailable = 8;
    static constexpr int targetAvailable = 16;
    KeyPackageSupply(ProfileSession &session, RelayClient &relay, QObject *parent = nullptr);
    void start(int available = -1);
    void pause();

private:
    void check();
    void receivedCount(int available);
    void publishNext();
    void retry();

    ProfileSession &m_session;
    RelayClient &m_relay;
    QTimer m_checkTimer;
    QTimer m_retryTimer;
    bool m_running = false;
    bool m_checking = false;
    bool m_publishing = false;
    bool m_filling = false;
    bool m_publishCountReceived = false;
    int m_available = -1;
};
} // namespace OpenChat
