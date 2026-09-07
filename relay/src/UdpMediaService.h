#pragma once

#include "domain/Identifiers.h"
#include "protocol/UdpMediaFrame.h"

#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QObject>

#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QUdpSocket;
class QNetworkDatagram;
class QTimer;
QT_END_NAMESPACE

namespace OpenChat::Relay {

// Relay UDP media forwarding service.
//
// Relays E2E sealed audio packets between authenticated devices without ever holding
// media keys or decrypting payloads.
//
// Security & Lifecycle:
//   - Media tokens are single-use, 60s TTL, 32 random bytes minted over authenticated WS.
//   - Hello consumes a token and pins senderDeviceId -> (addr, port).
//   - Media requires datagram source address/port == pinned binding.
//   - Bindings expire after 30s of silence.
//   - Per-binding token-bucket rate limit.
//   - Bye or WS disconnect clears the binding.
//   - Re-Hello re-pins (handles network roaming).
//   - No DB writes anywhere on this path.
class UdpMediaService final : public QObject
{
    Q_OBJECT

public:
    static constexpr qint64 tokenTtlMs = 60'000;
    static constexpr qint64 silenceTimeoutMs = 30'000;
    static constexpr double rateLimitCapacity = 150.0;
    static constexpr double rateLimitRefillPerSec = 100.0;

    explicit UdpMediaService(QObject *parent = nullptr);
    ~UdpMediaService() override;

    // Binds the UDP socket. Returns the bound port or 0 on failure.
    [[nodiscard]] quint16 start(const QHostAddress &address, quint16 port = 0);
    void stop();

    [[nodiscard]] quint16 localPort() const;
    [[nodiscard]] bool isRunning() const;

    // Mints a 32-byte single-use token with 60s TTL for the given device.
    [[nodiscard]] QByteArray mintToken(const DeviceId &device);

    // Clears any pinned binding for the device (called on Bye or WS disconnect).
    void clearBinding(const DeviceId &device);

    // Injectable clock for deterministic tests.
    void setClock(std::function<qint64()> clock) { m_clock = std::move(clock); }
    [[nodiscard]] qint64 nowMs() const;

    // Prunes expired tokens and silent bindings.
    void prune();

    // Inspection for tests
    [[nodiscard]] bool isBound(const DeviceId &device) const;
    [[nodiscard]] int activeBindingCount() const;
    [[nodiscard]] int pendingTokenCount() const;

private slots:
    void onReadyRead();

private:
    struct PendingToken final {
        DeviceId deviceId;
        qint64 expiresAtMs = 0;

        PendingToken(DeviceId id, qint64 expires)
            : deviceId(std::move(id))
            , expiresAtMs(expires)
        {
        }
    };

    struct Binding final {
        QHostAddress address;
        quint16 port = 0;
        qint64 lastSeenMs = 0;
        double tokens = rateLimitCapacity;
        qint64 lastRefillMs = 0;
    };

    void handleDatagram(const QNetworkDatagram &datagram);
    [[nodiscard]] bool checkRateLimit(Binding &binding, qint64 now);
    [[nodiscard]] static bool addressesMatch(const QHostAddress &a, const QHostAddress &b);

    std::unique_ptr<QUdpSocket> m_socket;
    QTimer *m_pruneTimer = nullptr;
    std::function<qint64()> m_clock;

    // 32-byte token -> PendingToken
    QHash<QByteArray, PendingToken> m_pendingTokens;
    // DeviceId -> Binding
    QHash<DeviceId, Binding> m_bindings;
};

} // namespace OpenChat::Relay
