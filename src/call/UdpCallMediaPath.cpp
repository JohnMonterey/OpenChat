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

void UdpCallMediaPath::ensureSocketBound()
{
    if (m_socket && m_socket->state() == QAbstractSocket::BoundState)
        return;

    m_socket = std::make_unique<QUdpSocket>(this);
    m_socket->bind(QHostAddress::AnyIPv4, 0);
    connect(m_socket.get(), &QUdpSocket::readyRead, this, &UdpCallMediaPath::onReadyRead);
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

    if (!m_lastToken.isEmpty()) {
        sendHello(m_lastToken);
        sendPing(p);
    } else if (m_relayClient) {
        m_relayClient->requestMediaToken();
    }
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
    return written == encoded.size();
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

void UdpCallMediaPath::onTokenReceived(const QByteArray &token)
{
    onRelayTokenReceived(token);
}

void UdpCallMediaPath::onRelayTokenReceived(const QByteArray &token)
{
    m_lastToken = token;
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
                qCDebug(mediaLog) << "UDP path to peer" << peer.peerDevice.toHex()
                                  << "suspended (silence >= 3s), falling back to WS";
            } else if (current - peer.lastPingSentMs >= pingIntervalMs) {
                sendPing(peer);
            }
        } else if (peer.state == UdpPeerState::Probing || peer.state == UdpPeerState::Suspended) {
            if (current - peer.lastProbeSentMs >= reprobeIntervalMs) {
                peer.lastProbeSentMs = current;
                peer.waitingForToken = true;
                if (!m_lastToken.isEmpty()) {
                    sendHello(m_lastToken);
                    sendPing(peer);
                } else if (m_relayClient) {
                    m_relayClient->requestMediaToken();
                }
            }
        }
    }
}

void UdpCallMediaPath::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket->receiveDatagram();
        handleDatagram(datagram.data());
    }
}

void UdpCallMediaPath::handleDatagram(const QByteArray &datagram)
{
    const auto decoded = UdpMediaFrame::decode(datagram);
    if (!decoded.has_value())
        return;

    const auto senderId = decoded->senderId();
    const qint64 current = nowMs();

    if (!senderId) {
        if (decoded->type == UdpMediaFrame::Type::HelloOk) {
            for (auto &peer : m_peers) {
                peer.helloSent = true;
                sendPing(peer);
            }
        }
        return;
    }

    auto it = m_peers.find(*senderId);
    if (it == m_peers.end())
        return;

    PeerInfo &peer = it.value();
    peer.lastPacketReceivedMs = current;

    switch (decoded->type) {
    case UdpMediaFrame::Type::Media: {
        if (peer.state != UdpPeerState::Active) {
            peer.state = UdpPeerState::Active;
            emit peerStateChanged(*senderId, UdpPeerState::Active);
            qCDebug(mediaLog) << "UDP path to peer" << senderId->toHex() << "became Active";
        }
        emit mediaReceived(*senderId, decoded->payload);
        return;
    }
    case UdpMediaFrame::Type::Ping: {
        if (peer.state != UdpPeerState::Active) {
            peer.state = UdpPeerState::Active;
            emit peerStateChanged(*senderId, UdpPeerState::Active);
            qCDebug(mediaLog) << "UDP path to peer" << senderId->toHex() << "became Active";
        }
        // Reply with Pong carrying echoed payload
        const auto pong = UdpMediaFrame::makePong(m_localDeviceId, *senderId, decoded->payload);
        const auto encoded = pong.encode();
        if (!encoded.isEmpty())
            m_socket->writeDatagram(encoded, m_relayAddress, m_relayPort);
        return;
    }
    case UdpMediaFrame::Type::Pong: {
        if (peer.state != UdpPeerState::Active) {
            peer.state = UdpPeerState::Active;
            emit peerStateChanged(*senderId, UdpPeerState::Active);
            qCDebug(mediaLog) << "UDP path to peer" << senderId->toHex() << "became Active";
        }
        if (decoded->payload.size() == sizeof(qint64)) {
            qint64 sentMs = 0;
            std::memcpy(&sentMs, decoded->payload.constData(), sizeof(qint64));
            const qint64 sample = current - sentMs;
            if (sample >= 0) {
                constexpr double alpha = 0.125;
                if (!peer.rttInitialized) {
                    peer.rttMs = static_cast<double>(sample);
                    peer.rttInitialized = true;
                } else {
                    peer.rttMs = (1.0 - alpha) * peer.rttMs + alpha * static_cast<double>(sample);
                }
                emit rttChanged(*senderId, peer.rttMs);
            }
        }
        return;
    }
    case UdpMediaFrame::Type::Bye: {
        peer.state = UdpPeerState::Suspended;
        emit peerStateChanged(*senderId, UdpPeerState::Suspended);
        return;
    }
    case UdpMediaFrame::Type::Hello:
    case UdpMediaFrame::Type::HelloOk:
    case UdpMediaFrame::Type::HelloErr:
        return;
    }
}

} // namespace OpenChat
