#include "UdpMediaService.h"

#include <QDateTime>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>

namespace OpenChat::Relay {

UdpMediaService::UdpMediaService(QObject *parent)
    : QObject(parent)
    , m_pruneTimer(new QTimer(this))
{
    m_pruneTimer->setInterval(5000);
    connect(m_pruneTimer, &QTimer::timeout, this, &UdpMediaService::prune);
}

UdpMediaService::~UdpMediaService()
{
    stop();
}

quint16 UdpMediaService::start(const QHostAddress &address, quint16 port)
{
    stop();

    m_socket = std::make_unique<QUdpSocket>(this);
    if (!m_socket->bind(address, port)) {
        m_socket.reset();
        return 0;
    }

    connect(m_socket.get(), &QUdpSocket::readyRead, this, &UdpMediaService::onReadyRead);
    m_pruneTimer->start();
    return m_socket->localPort();
}

void UdpMediaService::stop()
{
    if (m_pruneTimer)
        m_pruneTimer->stop();
    if (m_socket) {
        m_socket->close();
        m_socket.reset();
    }
    m_bindings.clear();
    m_pendingTokens.clear();
}

quint16 UdpMediaService::localPort() const
{
    return m_socket ? m_socket->localPort() : 0;
}

bool UdpMediaService::isRunning() const
{
    return m_socket && m_socket->state() == QAbstractSocket::BoundState;
}

qint64 UdpMediaService::nowMs() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

QByteArray UdpMediaService::mintToken(const DeviceId &device)
{
    QByteArray token(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(token.data()),
                                         token.size() / sizeof(quint32));

    m_pendingTokens.insert(token, PendingToken(device, nowMs() + tokenTtlMs));
    return token;
}

void UdpMediaService::clearBinding(const DeviceId &device)
{
    m_bindings.remove(device);
}

void UdpMediaService::prune()
{
    const qint64 current = nowMs();

    // Prune expired tokens
    for (auto it = m_pendingTokens.begin(); it != m_pendingTokens.end();) {
        if (current >= it->expiresAtMs)
            it = m_pendingTokens.erase(it);
        else
            ++it;
    }

    // Prune silent bindings
    for (auto it = m_bindings.begin(); it != m_bindings.end();) {
        if (current - it->lastSeenMs > silenceTimeoutMs)
            it = m_bindings.erase(it);
        else
            ++it;
    }
}

bool UdpMediaService::isBound(const DeviceId &device) const
{
    const auto it = m_bindings.find(device);
    if (it == m_bindings.end())
        return false;
    return (nowMs() - it->lastSeenMs <= silenceTimeoutMs);
}

int UdpMediaService::activeBindingCount() const
{
    int count = 0;
    const qint64 current = nowMs();
    for (const auto &b : m_bindings) {
        if (current - b.lastSeenMs <= silenceTimeoutMs)
            ++count;
    }
    return count;
}

int UdpMediaService::pendingTokenCount() const
{
    int count = 0;
    const qint64 current = nowMs();
    for (const auto &t : m_pendingTokens) {
        if (current < t.expiresAtMs)
            ++count;
    }
    return count;
}

bool UdpMediaService::addressesMatch(const QHostAddress &a, const QHostAddress &b)
{
    if (a == b)
        return true;
    return a.isEqual(b, QHostAddress::TolerantConversion);
}

bool UdpMediaService::checkRateLimit(Binding &binding, qint64 now)
{
    if (binding.lastRefillMs > 0 && now > binding.lastRefillMs) {
        const double elapsedSec = static_cast<double>(now - binding.lastRefillMs) / 1000.0;
        binding.tokens = std::min(rateLimitCapacity,
                                  binding.tokens + elapsedSec * rateLimitRefillPerSec);
    }
    binding.lastRefillMs = now;

    if (binding.tokens >= 1.0) {
        binding.tokens -= 1.0;
        return true;
    }
    return false;
}

void UdpMediaService::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QNetworkDatagram datagram = m_socket->receiveDatagram();
        handleDatagram(datagram);
    }
}

void UdpMediaService::handleDatagram(const QNetworkDatagram &datagram)
{
    const QByteArray data = datagram.data();
    if (data.size() > UdpMediaFrame::maxDatagramBytes)
        return;

    const auto decoded = UdpMediaFrame::decode(data);
    if (!decoded.has_value())
        return;

    const auto senderId = decoded->senderId();
    const qint64 current = nowMs();

    switch (decoded->type) {
    case UdpMediaFrame::Type::Hello: {
        if (!senderId)
            return;

        const QByteArray token = decoded->payload;
        const auto tokenIt = m_pendingTokens.find(token);
        if (tokenIt == m_pendingTokens.end() || current >= tokenIt->expiresAtMs
            || tokenIt->deviceId != *senderId) {
            const auto err = UdpMediaFrame::makeHelloErr(*senderId, "token invalid or expired");
            m_socket->writeDatagram(err.encode(), datagram.senderAddress(),
                                    static_cast<quint16>(datagram.senderPort()));
            return;
        }

        // Single-use token: consume immediately
        m_pendingTokens.erase(tokenIt);

        // Pin binding (re-Hello re-pins for network roaming)
        Binding &b = m_bindings[*senderId];
        b.address = datagram.senderAddress();
        b.port = static_cast<quint16>(datagram.senderPort());
        b.lastSeenMs = current;
        b.tokens = rateLimitCapacity;
        b.lastRefillMs = current;

        const auto ok = UdpMediaFrame::makeHelloOk(*senderId);
        m_socket->writeDatagram(ok.encode(), b.address, b.port);
        return;
    }
    case UdpMediaFrame::Type::Media:
    case UdpMediaFrame::Type::Ping:
    case UdpMediaFrame::Type::Pong: {
        if (!senderId)
            return;

        auto senderIt = m_bindings.find(*senderId);
        if (senderIt == m_bindings.end())
            return;

        // Verify source address & port match pinned binding
        if (!addressesMatch(senderIt->address, datagram.senderAddress())
            || senderIt->port != datagram.senderPort())
            return;

        // Verify binding has not expired
        if (current - senderIt->lastSeenMs > silenceTimeoutMs) {
            m_bindings.erase(senderIt);
            return;
        }

        senderIt->lastSeenMs = current;

        // Check rate limit
        if (!checkRateLimit(*senderIt, current))
            return;

        // Route to recipient
        const auto recipientId = decoded->recipientId();
        if (!recipientId)
            return;

        auto recipIt = m_bindings.find(*recipientId);
        if (recipIt == m_bindings.end())
            return;

        if (current - recipIt->lastSeenMs > silenceTimeoutMs) {
            m_bindings.erase(recipIt);
            return;
        }

        // Forward frame verbatim to recipient's pinned endpoint
        m_socket->writeDatagram(data, recipIt->address, recipIt->port);
        return;
    }
    case UdpMediaFrame::Type::Bye: {
        if (!senderId)
            return;

        auto senderIt = m_bindings.find(*senderId);
        if (senderIt != m_bindings.end()) {
            if (addressesMatch(senderIt->address, datagram.senderAddress())
                && senderIt->port == datagram.senderPort()) {
                m_bindings.erase(senderIt);
            }
        }

        // Forward Bye to recipient if bound
        const auto recipientId = decoded->recipientId();
        if (recipientId) {
            auto recipIt = m_bindings.find(*recipientId);
            if (recipIt != m_bindings.end() && current - recipIt->lastSeenMs <= silenceTimeoutMs) {
                m_socket->writeDatagram(data, recipIt->address, recipIt->port);
            }
        }
        return;
    }
    case UdpMediaFrame::Type::HelloOk:
    case UdpMediaFrame::Type::HelloErr:
        // Relay ignores incoming server-to-client responses
        return;
    }
}

} // namespace OpenChat::Relay
