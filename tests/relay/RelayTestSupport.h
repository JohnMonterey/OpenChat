#pragma once

// Test-only support for RelayClient hostile-transport tests. Everything here is
// confined to the test target and must never be linked into the application or
// any Qt resource. All certificates/keys are minted at runtime and marked
// "TEST-ONLY"; nothing secret is committed.

#include <QByteArray>
#include <QHash>
#include <QQueue>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>

#include <functional>

QT_BEGIN_NAMESPACE
class QSslServer;
class QSslSocket;
class QWebSocket;
class QWebSocketServer;
QT_END_NAMESPACE

namespace RelayTest {

// A self-signed, throwaway certificate authority that mints leaf certificates
// for the local fake servers. Uses the OpenSSL 3 C API directly.
class CertAuthority final
{
public:
    struct Leaf final {
        QByteArray certPem;
        QByteArray keyPem;
    };

    CertAuthority();
    ~CertAuthority();

    CertAuthority(const CertAuthority &) = delete;
    CertAuthority &operator=(const CertAuthority &) = delete;

    [[nodiscard]] QByteArray caCertPem() const { return m_caPem; }

    // sanSpec is an OpenSSL SAN string, e.g. "DNS:localhost,IP:127.0.0.1".
    [[nodiscard]] Leaf makeLeaf(const QByteArray &sanSpec, bool expired) const;

    [[nodiscard]] Leaf localhostLeaf() const { return makeLeaf("DNS:localhost,IP:127.0.0.1", false); }
    [[nodiscard]] Leaf wrongHostLeaf() const { return makeLeaf("DNS:wrong.invalid", false); }
    [[nodiscard]] Leaf expiredLeaf() const { return makeLeaf("DNS:localhost,IP:127.0.0.1", true); }

private:
    struct Impl;
    Impl *m_impl = nullptr;
    QByteArray m_caPem;
};

// A server-side TLS configuration presenting the given leaf. The server does not
// request a client certificate (VerifyNone) — the posture under test is the
// client's verification, not mutual TLS.
[[nodiscard]] QSslConfiguration serverConfig(const CertAuthority::Leaf &leaf);

// A client TLS configuration that trusts the given CA in addition to the system
// roots and asserts VerifyPeer + SecureProtocols. Never replaces the system
// roots and never disables verification.
[[nodiscard]] QSslConfiguration clientConfigTrusting(const QByteArray &caPem);

// ---------------------------------------------------------------------------

// A minimal fake WSS relay. Negotiates the configured subprotocols and hands the
// server-side QWebSocket to onConnected so a test can drive frames.
class FakeWssServer final
{
public:
    FakeWssServer(const QSslConfiguration &config, const QStringList &subprotocols);
    ~FakeWssServer();

    FakeWssServer(const FakeWssServer &) = delete;
    FakeWssServer &operator=(const FakeWssServer &) = delete;

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QUrl liveUrl(const QString &host = QStringLiteral("localhost")) const;

    // Invoked (once per accepted connection) with the server-side socket.
    std::function<void(QWebSocket *)> onConnected;

    // The request URL observed on the most recent upgrade (carries ?since=).
    [[nodiscard]] QUrl observedResumeUrl() const { return m_resumeUrl; }
    // The most recent binary message the server received from the client.
    [[nodiscard]] QByteArray lastClientBinary() const { return m_lastClientBinary; }

private:
    QWebSocketServer *m_server = nullptr;
    QWebSocket *m_socket = nullptr;
    QUrl m_resumeUrl;
    QByteArray m_lastClientBinary;
};

// A minimal fake HTTPS relay that serves canned responses per path (FIFO) and
// records requests. Responses are scripted so a path can return 401 then 200.
class FakeHttpsServer final
{
public:
    struct Response final {
        int status = 200;
        QByteArray body;
        // When >= 0, advertise this Content-Length instead of body.size() and
        // send only the short body — used to exercise the declared-oversize
        // guard without transferring huge payloads.
        qint64 declaredLength = -1;
    };

    explicit FakeHttpsServer(const QSslConfiguration &config);
    ~FakeHttpsServer();

    FakeHttpsServer(const FakeHttpsServer &) = delete;
    FakeHttpsServer &operator=(const FakeHttpsServer &) = delete;

    [[nodiscard]] bool isListening() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QUrl url(const QString &path, const QString &host = QStringLiteral("localhost")) const;

    void enqueue(const QString &path, Response response);

    [[nodiscard]] int requestCount(const QString &path) const;
    [[nodiscard]] QByteArray lastAuthorization(const QString &path) const;
    [[nodiscard]] QByteArray lastBody(const QString &path) const;
    // The full request target (path plus any query string) of the most recent
    // request to `path`. Lets a test observe a GET's query items, which are
    // stripped from the key used everywhere else.
    [[nodiscard]] QByteArray lastTarget(const QString &path) const;

private:
    void onReadyRead(QSslSocket *socket);
    void dispatch(QSslSocket *socket, const QByteArray &requestLine, const QByteArray &headers,
                  const QByteArray &body);

    QSslServer *m_server = nullptr;
    QHash<QSslSocket *, QByteArray> m_buffers;
    QHash<QString, QQueue<Response>> m_responses;
    QHash<QString, int> m_requestCounts;
    QHash<QString, QByteArray> m_lastAuthorization;
    QHash<QString, QByteArray> m_lastBody;
    QHash<QString, QByteArray> m_lastTarget;
};

} // namespace RelayTest
