#pragma once

#include "domain/Identifiers.h"
#include "protocol/UdpMediaFrame.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QUdpSocket;
class QTimer;
QT_END_NAMESPACE

namespace OpenChat {

class RelayClient;
class TransportSettings;

enum class UdpPeerState {
    Probing,
    Active,
    Suspended,
};

// Main-thread UDP media transport for real-time voice calls with automatic WebSocket fallback.
//
// Lifecycle & Failover:
//   - Probes the relay UDP endpoint per peer while voice media initially flows over WS.
//   - When the first authenticated UDP packet (Pong or Media) arrives, the peer becomes Active.
//   - Voice packets (audio version 1) divert to UDP while the peer is Active.
//   - If UDP packet silence reaches >= 3s, the peer suspends to WS and re-probes every 5s.
//   - Periodic Ping every 5s maintains NAT mappings and tracks RTT EMA.
//   - Bye is sent on hangup.
//   - Follows TransportSettings: mode auto (UDP with WS fallback), udp, or tcp (WS only).
class UdpCallMediaPath final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 lossTimeoutMs = 3000;
    static constexpr qint64 pingIntervalMs = 5000;
    static constexpr qint64 reprobeIntervalMs = 5000;

    explicit UdpCallMediaPath(const DeviceId &localDeviceId, QObject *parent = nullptr);
    ~UdpCallMediaPath() override;

    void setLocalDeviceId(const DeviceId &device);
    [[nodiscard]] DeviceId localDeviceId() const { return m_localDeviceId; }

    void setRelayEndpoint(const QHostAddress &address, quint16 port);
    [[nodiscard]] QHostAddress relayAddress() const { return m_relayAddress; }
    [[nodiscard]] quint16 relayPort() const { return m_relayPort; }

    void setRelayClient(RelayClient *client);
    void setSettings(TransportSettings *settings);

    // Starts probing UDP path for the peer.
    void startProbing(const DeviceId &peer);
    // Stops UDP path for the peer and sends Bye.
    void stop(const DeviceId &peer);
    void stopAll();

    // Sends Bye to the peer.
    void sendBye(const DeviceId &peer);

    // Sends an audio packet over UDP to the peer if the peer path is Active.
    // Returns true if sent over UDP, false if caller should use WS route.
    bool sendMedia(const DeviceId &recipient, const QByteArray &packet);

    [[nodiscard]] bool isPeerActive(const DeviceId &peer) const;
    [[nodiscard]] UdpPeerState peerState(const DeviceId &peer) const;
    [[nodiscard]] double rttMs(const DeviceId &peer) const;
    [[nodiscard]] QString mediaPathText(const DeviceId &peer) const;

    // Injectable clock for testing.
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    [[nodiscard]] qint64 nowMs() const;

    // Direct token injection for tests without RelayClient.
    void onTokenReceived(const QByteArray &token);

    // Socket inspection for tests
    [[nodiscard]] QUdpSocket *socket() const { return m_socket.get(); }
    [[nodiscard]] quint16 localPort() const;

signals:
    void mediaReceived(const DeviceId &sender, const QByteArray &packet);
    void peerStateChanged(const DeviceId &peer, UdpPeerState state);
    void rttChanged(const DeviceId &peer, double rttMs);

private slots:
    void onReadyRead();
    void onTickerTimeout();
    void onRelayTokenReceived(const QByteArray &token);
    void onRelayTokenFailed();

private:
    struct PeerInfo final {
        DeviceId peerDevice;
        UdpPeerState state = UdpPeerState::Probing;
        qint64 lastPacketReceivedMs = 0;
        qint64 lastProbeSentMs = 0;
        qint64 lastPingSentMs = 0;
        double rttMs = 0.0;
        bool rttInitialized = false;
        bool helloSent = false;
        bool waitingForToken = false;

        PeerInfo(DeviceId id) : peerDevice(std::move(id)) {}
    };

    void ensureSocketBound();
    void sendHello(const QByteArray &token);
    void sendPing(PeerInfo &peer);
    void handleDatagram(const QByteArray &datagram);

    DeviceId m_localDeviceId;
    QHostAddress m_relayAddress = QHostAddress::LocalHost;
    quint16 m_relayPort = 8444;

    RelayClient *m_relayClient = nullptr;
    TransportSettings *m_settings = nullptr;

    std::unique_ptr<QUdpSocket> m_socket;
    QTimer *m_ticker = nullptr;
    std::function<qint64()> m_clock;

    QHash<DeviceId, PeerInfo> m_peers;
    QByteArray m_lastToken;
};

} // namespace OpenChat
