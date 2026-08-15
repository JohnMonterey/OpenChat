#pragma once

#include "AuthService.h"
#include "EnvelopeService.h"
#include "KeyPackageService.h"
#include "PostgresStore.h"

#include <QHash>
#include <QHttpServer>
#include <QHostAddress>
#include <QObject>

#include <memory>

QT_BEGIN_NAMESPACE
class QTcpServer;
class QWebSocket;
QT_END_NAMESPACE

namespace OpenChat::Relay {

// HTTP + WebSocket front end for the relay services. Plain HTTP/WS on a loopback
// interface behind a TLS-terminating reverse proxy. Every request body is
// bounded before parsing; the live socket carries only opaque frames.
class RelayServer final : public QObject
{
    Q_OBJECT

public:
    struct Limits final {
        qint64 maxRequestBytes = 1 * 1024 * 1024;   // per HTTP request body
        quint64 maxFrameBytes = 1 * 1024 * 1024;    // per WebSocket message
        int syncLimit = 256;                        // catch-up page size
    };

    RelayServer(PostgresStore &store, AuthService &auth, EnvelopeService &envelopes,
                KeyPackageService &keyPackages, QObject *parent = nullptr);
    RelayServer(PostgresStore &store, AuthService &auth, EnvelopeService &envelopes,
                KeyPackageService &keyPackages, Limits limits, QObject *parent);
    ~RelayServer() override;

    // Binds a loopback listener and registers routes. Returns the bound port, or
    // 0 on failure.
    [[nodiscard]] quint16 start(const QHostAddress &address, quint16 port);

private:
    void registerRoutes();
    void onWebSocketConnection();
    void handleLiveBinary(QWebSocket *socket, const AuthenticatedDevice &device,
                          const QByteArray &message);

    PostgresStore &m_store;
    AuthService &m_auth;
    EnvelopeService &m_envelopes;
    KeyPackageService &m_keyPackages;
    Limits m_limits;

    QHttpServer m_http;
    QTcpServer *m_tcp = nullptr;
    // Live sockets keyed by recipient device id bytes (hex), for best-effort
    // real-time delivery of freshly accepted envelopes.
    QHash<QByteArray, QWebSocket *> m_liveByDevice;
};

} // namespace OpenChat::Relay
