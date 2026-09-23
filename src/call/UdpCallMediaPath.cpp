#include "call/UdpCallMediaPath.h"

#include "app/TransportSettings.h"
#include "diagnostics/Logging.h"
#include "network/RelayClient.h"

#include <QDateTime>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#include <cstring>

namespace OpenChat {

UdpCallMediaPath::UdpCallMediaPath(const DeviceId &localDeviceId, QObject *parent)
    : QObject(parent)
    , m_localDeviceId(localDeviceId)
    , m_ticker(new QTimer(this))
{
    m_ticker->setInterval(500);
    connect(m_ticker, &QTimer::timeout, this, &UdpCallMediaPath::onTickerTimeout);
}

UdpCallMediaPath::~UdpCallMediaPath()
{
    stopAll();
}

void UdpCallMediaPath::setLocalDeviceId(const DeviceId &device)
{
    m_localDeviceId = device;
}

void UdpCallMediaPath::setRelayEndpoint(const QHostAddress &address, quint16 port)
{
    m_relayAddress = address;
    m_relayPort = port;
}

void UdpCallMediaPath::setRelayClient(RelayClient *client)
{
    if (m_relayClient == client)
        return;

    if (m_relayClient) {
        disconnect(m_relayClient, &RelayClient::mediaTokenReceived, this,
                   &UdpCallMediaPath::onRelayTokenReceived);
        disconnect(m_relayClient, &RelayClient::mediaTokenFailed, this,
                   &UdpCallMediaPath::onRelayTokenFailed);
    }

    m_relayClient = client;
    if (m_relayClient) {
        connect(m_relayClient, &RelayClient::mediaTokenReceived, this,
                &UdpCallMediaPath::onRelayTokenReceived);
        connect(m_relayClient, &RelayClient::mediaTokenFailed, this,
                &UdpCallMediaPath::onRelayTokenFailed);
    }
}

void UdpCallMediaPath::setSettings(TransportSettings *settings)
{
    m_settings = settings;
}

quint16 UdpCallMediaPath::localPort() const
{
    return m_socket ? m_socket->localPort() : 0;
}

qint64 UdpCallMediaPath::nowMs() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void UdpCallMediaPath::logDiagnostic(const QString &category, const QString &message,
                                     const QString &severity)
{
    emit diagnosticEventLogged(category, message, severity);
    if (severity == QLatin1String("ERROR") || severity == QLatin1String("FAILOVER"))
        qCWarning(mediaLog).noquote() << category << message;
    else
        qCInfo(mediaLog).noquote() << category << message;
}

void UdpCallMediaPath::ensureSocketBound()
{
    if (m_socket && m_socket->state() == QAbstractSocket::BoundState)
        return;

    m_socket = std::make_unique<QUdpSocket>(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, 0)) {
        // Without a socket there is no UDP path at all, and every send below
        // silently does nothing; say so once rather than falling back mutely.
        logDiagnostic(QStringLiteral("UDP"),
                      QStringLiteral("Failed to bind local UDP socket: %1")
                          .arg(m_socket->errorString()),
                      QStringLiteral("ERROR"));
        return;
    }
    connect(m_socket.get(), &QUdpSocket::readyRead, this, &UdpCallMediaPath::onReadyRead);
    connect(m_socket.get(), &QUdpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                logDiagnostic(QStringLiteral("UDP"),
                              QStringLiteral("UDP socket error: %1").arg(m_socket->errorString()),
                              QStringLiteral("ERROR"));
            });
    logDiagnostic(QStringLiteral("UDP"),
                  QStringLiteral("Bound local UDP socket on port %1, relay %2:%3")
                      .arg(m_socket->localPort())
                      .arg(m_relayAddress.toString())
                      .arg(m_relayPort),
                  QStringLiteral("INFO"));
}

void UdpCallMediaPath::requestToken(const QString &reason)
{
    if (!m_relayClient) {
        logDiagnostic(QStringLiteral("AUTH"),
                      QStringLiteral("Cannot request media token (%1): no relay client").arg(reason),
                      QStringLiteral("ERROR"));
        return;
    }

    const qint64 current = nowMs();
    if (m_lastTokenRequestMs != 0 && current - m_lastTokenRequestMs < minTokenRequestIntervalMs)
        return;

    m_lastTokenRequestMs = current;
    logDiagnostic(QStringLiteral("AUTH"),
                  QStringLiteral("Requesting media token from relay (%1)").arg(reason),
                  QStringLiteral("INFO"));
    m_relayClient->requestMediaToken();
}

void UdpCallMediaPath::startProbing(const DeviceId &peer)
{
    if (m_settings && m_settings->transportMode() == TransportMode::Tcp)
        return;

    ensureSocketBound();

    if (!m_ticker->isActive())
        m_ticker->start();

    auto it = m_peers.find(peer);
    if (it == m_peers.end()) {
        it = m_peers.insert(peer, PeerInfo(peer));
    }

    PeerInfo &p = it.value();
    p.state = UdpPeerState::Probing;
    p.lastProbeSentMs = nowMs();
    p.waitingForToken = true;

    // A ping costs one datagram and reaches the peer immediately if this device
    // is still bound from an earlier call; the token below re-binds it if not.
    sendPing(p);
    requestToken(QStringLiteral("probing peer %1").arg(peer.toHex().left(8)));
}

void UdpCallMediaPath::stop(const DeviceId &peer)
{
    sendBye(peer);
    m_peers.remove(peer);
    if (m_peers.isEmpty() && m_ticker->isActive())
        m_ticker->stop();
}

void UdpCallMediaPath::stopAll()
{
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        sendBye(it.key());
    }
    m_peers.clear();
    if (m_ticker)
        m_ticker->stop();
    if (m_socket) {
        m_socket->close();
        m_socket.reset();
    }
}

void UdpCallMediaPath::sendHello(const QByteArray &token)
{
    ensureSocketBound();
    const auto hello = UdpMediaFrame::makeHello(m_localDeviceId, token);
    const auto encoded = hello.encode();
    if (!encoded.isEmpty()) {
        m_socket->writeDatagram(encoded, m_relayAddress, m_relayPort);
        m_totalBytesSent += encoded.size();
        logDiagnostic(
            QStringLiteral("UDP"),
            QStringLiteral("Sent Hello to relay %1:%2 (token size %3 bytes)")
                .arg(m_relayAddress.toString())
                .arg(m_relayPort)
                .arg(token.size()),
            QStringLiteral("INFO"));
    }
}

void UdpCallMediaPath::sendPing(PeerInfo &peer)
{
    ensureSocketBound();
    qint64 current = nowMs();
    QByteArray payload(sizeof(qint64), Qt::Uninitialized);
    std::memcpy(payload.data(), &current, sizeof(qint64));

    const auto ping = UdpMediaFrame::makePing(m_localDeviceId, peer.peerDevice, payload);
    const auto encoded = ping.encode();
    if (!encoded.isEmpty()) {
        m_socket->writeDatagram(encoded, m_relayAddress, m_relayPort);
        peer.lastPingSentMs = current;
        peer.pingsSent++;
        peer.bytesSent += encoded.size();
        m_totalBytesSent += encoded.size();
    }
}

void UdpCallMediaPath::sendBye(const DeviceId &peer)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::BoundState)
        return;

    const auto bye = UdpMediaFrame::makeBye(m_localDeviceId, peer);
    const auto encoded = bye.encode();
    if (!encoded.isEmpty()) {
        m_socket->writeDatagram(encoded, m_relayAddress, m_relayPort);
        m_totalBytesSent += encoded.size();
        logDiagnostic(
            QStringLiteral("CARRIER"),
            QStringLiteral("Sent Bye for peer %1").arg(peer.toHex().left(8)),
            QStringLiteral("INFO"));
    }
}

bool UdpCallMediaPath::sendMedia(const DeviceId &recipient, const QByteArray &packet)
{
    if (!isPeerActive(recipient))
        return false;

    ensureSocketBound();
    const auto media = UdpMediaFrame::makeMedia(m_localDeviceId, recipient, packet);
    const auto encoded = media.encode();
    if (encoded.isEmpty())
        return false;

    const qint64 written = m_socket->writeDatagram(encoded, m_relayAddress, m_relayPort);
    if (written == encoded.size()) {
        auto it = m_peers.find(recipient);
        if (it != m_peers.end()) {
            it->mediaPacketsSent++;
            it->bytesSent += written;
        }
        m_totalBytesSent += written;
        return true;
    }
    return false;
}

bool UdpCallMediaPath::isPeerActive(const DeviceId &peer) const
{
    if (m_settings && m_settings->transportMode() == TransportMode::Tcp)
        return false;

    const auto it = m_peers.find(peer);
    if (it == m_peers.end())
        return false;
    return it->state == UdpPeerState::Active;
}

UdpPeerState UdpCallMediaPath::peerState(const DeviceId &peer) const
{
    const auto it = m_peers.find(peer);
    if (it == m_peers.end())
        return UdpPeerState::Probing;
    return it->state;
}

double UdpCallMediaPath::rttMs(const DeviceId &peer) const
{
    const auto it = m_peers.find(peer);
    if (it == m_peers.end())
        return 0.0;
    return it->rttMs;
}

QString UdpCallMediaPath::mediaPathText(const DeviceId &peer) const
{
    if (m_settings && m_settings->transportMode() == TransportMode::Tcp)
        return QStringLiteral("Relay (TCP)");

    const auto it = m_peers.find(peer);
    if (it != m_peers.end() && it->state == UdpPeerState::Active) {
        return QStringLiteral("UDP · %1 ms").arg(qRound(it->rttMs));
    }
    return QStringLiteral("Relay (TCP)");
}

std::optional<DeviceId> UdpCallMediaPath::firstPeer() const
{
    if (m_peers.isEmpty())
        return std::nullopt;
    return m_peers.constBegin().key();
}

QList<DeviceId> UdpCallMediaPath::allPeers() const
{
    return m_peers.keys();
}

std::optional<UdpPeerTelemetry> UdpCallMediaPath::peerTelemetry(const DeviceId &peer) const
{
    const auto it = m_peers.find(peer);
    if (it == m_peers.end())
        return std::nullopt;

    const PeerInfo &p = it.value();
    UdpPeerTelemetry t;
    t.peer = p.peerDevice;
    t.state = p.state;
    t.rttMs = p.rttMs;
    t.lastRawRttMs = p.lastRawRttMs;
    t.minRttMs = p.minRttMs;
    t.maxRttMs = p.maxRttMs;
    t.pingsSent = p.pingsSent;
    t.pongsReceived = p.pongsReceived;
    t.unackedPings = p.pingsSent > p.pongsReceived ? (p.pingsSent - p.pongsReceived) : 0;
    t.mediaPacketsSent = p.mediaPacketsSent;
    t.mediaPacketsReceived = p.mediaPacketsReceived;
    t.bytesSent = p.bytesSent;
    t.bytesReceived = p.bytesReceived;
    t.lastPacketReceivedMs = p.lastPacketReceivedMs;
    t.silenceMs = p.lastPacketReceivedMs > 0 ? (nowMs() - p.lastPacketReceivedMs) : 0;
    t.lagSpikeCount = p.lagSpikeCount;
    return t;
}

void UdpCallMediaPath::triggerPing(const DeviceId &peer)
{
    auto it = m_peers.find(peer);
    if (it != m_peers.end()) {
        sendPing(it.value());
        logDiagnostic(
            QStringLiteral("PING"),
            QStringLiteral("Manual ping triggered to peer %1").arg(peer.toHex().left(8)),
            QStringLiteral("INFO"));
    }
}

void UdpCallMediaPath::onTokenReceived(const QByteArray &token)
{
    onRelayTokenReceived(token);
}

void UdpCallMediaPath::onRelayTokenReceived(const QByteArray &token)
{
    // Deliberately not cached: the relay consumes a token on the first Hello,
    // so the next Hello needs a newly minted one.
    logDiagnostic(QStringLiteral("AUTH"),
                  QStringLiteral("Received media token (%1 bytes) from relay").arg(token.size()),
                  QStringLiteral("INFO"));
    sendHello(token);

    for (auto &peer : m_peers) {
        if (peer.waitingForToken) {
            peer.waitingForToken = false;
            peer.helloSent = true;
            sendPing(peer);
        }
    }
}

void UdpCallMediaPath::onRelayTokenFailed()
{
    logDiagnostic(QStringLiteral("AUTH"),
                  QStringLiteral("Relay rejected or could not answer the media token request; "
                                 "voice stays on the WebSocket until the next re-probe"),
                  QStringLiteral("ERROR"));
    for (auto &peer : m_peers) {
        peer.waitingForToken = false;
    }
}

void UdpCallMediaPath::onTickerTimeout()
{
    const qint64 current = nowMs();

    for (auto &peer : m_peers) {
        if (peer.state == UdpPeerState::Active) {
            if (current - peer.lastPacketReceivedMs >= lossTimeoutMs) {
                peer.state = UdpPeerState::Suspended;
                peer.helloSent = false;
                emit peerStateChanged(peer.peerDevice, UdpPeerState::Suspended);
                logDiagnostic(
                    QStringLiteral("CARRIER"),
                    QStringLiteral("UDP silence reached %1 ms >= %2 ms: Suspended -> Falling back to WS")
                        .arg(current - peer.lastPacketReceivedMs)
                        .arg(lossTimeoutMs),
                    QStringLiteral("FAILOVER"));
                qCDebug(mediaLog) << "UDP path to peer" << peer.peerDevice.toHex()
                                  << "suspended (silence >= 3s), falling back to WS";
            } else if (current - peer.lastPingSentMs >= pingIntervalMs) {
                sendPing(peer);
            }
        } else if (peer.state == UdpPeerState::Probing || peer.state == UdpPeerState::Suspended) {
            if (current - peer.lastProbeSentMs >= reprobeIntervalMs) {
                peer.lastProbeSentMs = current;
                peer.waitingForToken = true;
                // Ping first: if this device is still bound the peer may simply
                // have come up late, and no new binding is needed at all.
                sendPing(peer);
                requestToken(QStringLiteral("re-probing peer %1 (state %2)")
                                 .arg(peer.peerDevice.toHex().left(8),
                                      peer.state == UdpPeerState::Suspended
                                          ? QStringLiteral("suspended")
                                          : QStringLiteral("probing")));
            }
        }
    }
}

void UdpCallMediaPath::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket->receiveDatagram();
        // Everything on this socket should come back from the relay we sent to.
        // Anything else is either a stale endpoint or an injected frame, and is
        // worth naming rather than silently folding into the peer's liveness.
        if (!datagram.senderAddress().isEqual(m_relayAddress, QHostAddress::TolerantConversion)) {
            logDiagnostic(QStringLiteral("UDP"),
                          QStringLiteral("Datagram from unexpected source %1:%2 (relay is %3:%4)")
                              .arg(datagram.senderAddress().toString())
                              .arg(datagram.senderPort())
                              .arg(m_relayAddress.toString())
                              .arg(m_relayPort),
                          QStringLiteral("ERROR"));
        }
        handleDatagram(datagram.data());
    }
}

void UdpCallMediaPath::handleDatagram(const QByteArray &datagram)
{
    m_totalBytesReceived += datagram.size();

    const auto decoded = UdpMediaFrame::decode(datagram);
    if (!decoded.has_value())
        return;

    const auto senderId = decoded->senderId();
    const qint64 current = nowMs();

    if (!senderId) {
        if (decoded->type == UdpMediaFrame::Type::HelloOk) {
            logDiagnostic(QStringLiteral("UDP"),
                          QStringLiteral("Relay acknowledged Hello (HelloOk); this device is bound"),
                          QStringLiteral("INFO"));
            for (auto &peer : m_peers) {
                peer.helloSent = true;
                sendPing(peer);
            }
        } else if (decoded->type == UdpMediaFrame::Type::HelloErr) {
            // The relay refused to bind us. Without this branch the refusal is
            // invisible and the path sits on the WebSocket forever.
            logDiagnostic(QStringLiteral("AUTH"),
                          QStringLiteral("Relay rejected Hello: %1")
                              .arg(QString::fromUtf8(decoded->payload)),
                          QStringLiteral("ERROR"));
            requestToken(QStringLiteral("relay rejected the previous Hello"));
        }
        return;
    }

    auto it = m_peers.find(*senderId);
    if (it == m_peers.end())
        return;

    PeerInfo &peer = it.value();
    peer.lastPacketReceivedMs = current;
    peer.bytesReceived += datagram.size();

    switch (decoded->type) {
    case UdpMediaFrame::Type::Media: {
        peer.mediaPacketsReceived++;
        if (peer.state != UdpPeerState::Active) {
            peer.state = UdpPeerState::Active;
            emit peerStateChanged(*senderId, UdpPeerState::Active);
            logDiagnostic(
                QStringLiteral("CARRIER"),
                QStringLiteral("Peer %1 UDP carrier is now ACTIVE via Media arrival")
                    .arg(senderId->toHex().left(8)),
                QStringLiteral("INFO"));
            qCDebug(mediaLog) << "UDP path to peer" << senderId->toHex() << "became Active";
        }
        emit mediaReceived(*senderId, decoded->payload);
        return;
    }
    case UdpMediaFrame::Type::Ping: {
        if (peer.state != UdpPeerState::Active) {
            peer.state = UdpPeerState::Active;
            emit peerStateChanged(*senderId, UdpPeerState::Active);
            logDiagnostic(
                QStringLiteral("CARRIER"),
                QStringLiteral("Peer %1 UDP carrier is now ACTIVE via Ping arrival")
                    .arg(senderId->toHex().left(8)),
                QStringLiteral("INFO"));
            qCDebug(mediaLog) << "UDP path to peer" << senderId->toHex() << "became Active";
        }
        // Reply with Pong carrying echoed payload
        const auto pong = UdpMediaFrame::makePong(m_localDeviceId, *senderId, decoded->payload);
        const auto encoded = pong.encode();
        if (!encoded.isEmpty()) {
            m_socket->writeDatagram(encoded, m_relayAddress, m_relayPort);
            m_totalBytesSent += encoded.size();
        }
        return;
    }
    case UdpMediaFrame::Type::Pong: {
        peer.pongsReceived++;
        if (peer.state != UdpPeerState::Active) {
            peer.state = UdpPeerState::Active;
            emit peerStateChanged(*senderId, UdpPeerState::Active);
            logDiagnostic(
                QStringLiteral("CARRIER"),
                QStringLiteral("Peer %1 UDP carrier is now ACTIVE via Pong reply")
                    .arg(senderId->toHex().left(8)),
                QStringLiteral("INFO"));
            qCDebug(mediaLog) << "UDP path to peer" << senderId->toHex() << "became Active";
        }
        if (decoded->payload.size() == sizeof(qint64)) {
            qint64 sentMs = 0;
            std::memcpy(&sentMs, decoded->payload.constData(), sizeof(qint64));
            const qint64 sample = current - sentMs;
            if (sample >= 0) {
                const double sampleMs = static_cast<double>(sample);
                peer.lastRawRttMs = sampleMs;
                constexpr double alpha = 0.125;
                if (!peer.rttInitialized) {
                    peer.rttMs = sampleMs;
                    peer.minRttMs = sampleMs;
                    peer.maxRttMs = sampleMs;
                    peer.rttInitialized = true;
                } else {
                    peer.minRttMs = std::min(peer.minRttMs, sampleMs);
                    peer.maxRttMs = std::max(peer.maxRttMs, sampleMs);

                    // Lag spike detection:
                    // If sample > 100ms AND (sample > 1.5 * baseline EMA or sample - baseline > 40ms)
                    if (sampleMs > 100.0 && (sampleMs > 1.5 * peer.rttMs || (sampleMs - peer.rttMs) > 40.0)) {
                        peer.lagSpikeCount++;
                        emit lagSpikeDetected(*senderId, sampleMs, peer.rttMs);
                        logDiagnostic(
                            QStringLiteral("SPIKE"),
                            QStringLiteral("LATENCY SPIKE DETECTED: %1 ms (baseline: %2 ms, delta: +%3 ms)")
                                .arg(sampleMs, 0, 'f', 1)
                                .arg(peer.rttMs, 0, 'f', 1)
                                .arg(sampleMs - peer.rttMs, 0, 'f', 1),
                            QStringLiteral("SPIKE"));
                    }

                    peer.rttMs = (1.0 - alpha) * peer.rttMs + alpha * sampleMs;
                }
                emit rttChanged(*senderId, peer.rttMs);
            }
        }
        return;
    }
    case UdpMediaFrame::Type::Bye: {
        peer.state = UdpPeerState::Suspended;
        emit peerStateChanged(*senderId, UdpPeerState::Suspended);
        logDiagnostic(
            QStringLiteral("CARRIER"),
            QStringLiteral("Peer %1 sent Bye; path suspended").arg(senderId->toHex().left(8)),
            QStringLiteral("INFO"));
        return;
    }
    case UdpMediaFrame::Type::Hello:
    case UdpMediaFrame::Type::HelloOk:
    case UdpMediaFrame::Type::HelloErr:
        return;
    }
}

} // namespace OpenChat
