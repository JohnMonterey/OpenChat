#include "relay/RelayTestSupport.h"

#include <QHostAddress>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace RelayTest {

namespace {

QByteArray bioToByteArray(BIO *bio)
{
    char *data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    return QByteArray(data, static_cast<int>(length));
}

QByteArray certToPem(X509 *cert)
{
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    const QByteArray pem = bioToByteArray(bio);
    BIO_free(bio);
    return pem;
}

QByteArray keyToPem(EVP_PKEY *key)
{
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    const QByteArray pem = bioToByteArray(bio);
    BIO_free(bio);
    return pem;
}

void addExtension(X509 *cert, X509 *issuer, X509 *subject, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, subject, nullptr, nullptr, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
}

} // namespace

struct CertAuthority::Impl {
    EVP_PKEY *caKey = nullptr;
    X509 *caCert = nullptr;
    mutable long serial = 2;

    ~Impl()
    {
        if (caCert)
            X509_free(caCert);
        if (caKey)
            EVP_PKEY_free(caKey);
    }
};

CertAuthority::CertAuthority()
    : m_impl(new Impl)
{
    m_impl->caKey = EVP_RSA_gen(2048);

    m_impl->caCert = X509_new();
    X509_set_version(m_impl->caCert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(m_impl->caCert), 1);
    X509_gmtime_adj(X509_getm_notBefore(m_impl->caCert), 0);
    X509_gmtime_adj(X509_getm_notAfter(m_impl->caCert), 60L * 60 * 24 * 365);

    X509_NAME *name = X509_get_subject_name(m_impl->caCert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>("OpenChat TEST-ONLY CA"),
                               -1, -1, 0);
    X509_set_issuer_name(m_impl->caCert, name);
    X509_set_pubkey(m_impl->caCert, m_impl->caKey);
    addExtension(m_impl->caCert, m_impl->caCert, m_impl->caCert, NID_basic_constraints,
                 "critical,CA:TRUE");
    X509_sign(m_impl->caCert, m_impl->caKey, EVP_sha256());

    m_caPem = certToPem(m_impl->caCert);
}

CertAuthority::~CertAuthority()
{
    delete m_impl;
}

CertAuthority::Leaf CertAuthority::makeLeaf(const QByteArray &sanSpec, bool expired) const
{
    EVP_PKEY *leafKey = EVP_RSA_gen(2048);
    X509 *leaf = X509_new();
    X509_set_version(leaf, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(leaf), m_impl->serial++);

    if (expired) {
        // Both bounds in the past, deterministic regardless of build date.
        X509_gmtime_adj(X509_getm_notBefore(leaf), -60L * 60 * 24 * 30);
        X509_gmtime_adj(X509_getm_notAfter(leaf), -60L * 60 * 24 * 1);
    } else {
        X509_gmtime_adj(X509_getm_notBefore(leaf), 0);
        X509_gmtime_adj(X509_getm_notAfter(leaf), 60L * 60 * 24 * 30);
    }

    X509_NAME *name = X509_get_subject_name(leaf);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("OpenChat TEST-ONLY localhost"), -1, -1, 0);
    X509_set_issuer_name(leaf, X509_get_subject_name(m_impl->caCert));
    X509_set_pubkey(leaf, leafKey);
    addExtension(leaf, m_impl->caCert, leaf, NID_basic_constraints, "critical,CA:FALSE");
    addExtension(leaf, m_impl->caCert, leaf, NID_subject_alt_name, sanSpec.constData());
    X509_sign(leaf, m_impl->caKey, EVP_sha256());

    Leaf result;
    result.certPem = certToPem(leaf);
    result.keyPem = keyToPem(leafKey);

    X509_free(leaf);
    EVP_PKEY_free(leafKey);
    return result;
}

QSslConfiguration serverConfig(const CertAuthority::Leaf &leaf)
{
    QSslConfiguration config;
    const QList<QSslCertificate> certs = QSslCertificate::fromData(leaf.certPem, QSsl::Pem);
    if (!certs.isEmpty())
        config.setLocalCertificate(certs.first());
    config.setPrivateKey(QSslKey(leaf.keyPem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey));
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    config.setProtocol(QSsl::SecureProtocols);
    return config;
}

QSslConfiguration clientConfigTrusting(const QByteArray &caPem)
{
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    const QList<QSslCertificate> cas = QSslCertificate::fromData(caPem, QSsl::Pem);
    for (const QSslCertificate &ca : cas)
        config.addCaCertificate(ca);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    config.setProtocol(QSsl::SecureProtocols);
    return config;
}

// ---------------------------------------------------------------------------

FakeWssServer::FakeWssServer(const QSslConfiguration &config, const QStringList &subprotocols)
{
    m_server = new QWebSocketServer(QStringLiteral("openchat-test"),
                                    QWebSocketServer::SecureMode);
    m_server->setSslConfiguration(config);
    m_server->setSupportedSubprotocols(subprotocols);
    m_server->listen(QHostAddress::LocalHost, 0);

    QObject::connect(m_server, &QWebSocketServer::newConnection, m_server, [this] {
        QWebSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            return;
        m_socket = socket;
        m_resumeUrl = socket->requestUrl();
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket,
                         [this](const QByteArray &message) { m_lastClientBinary = message; });
        if (onConnected)
            onConnected(socket);
    });
}

FakeWssServer::~FakeWssServer()
{
    if (m_server) {
        m_server->close();
        delete m_server;
    }
}

bool FakeWssServer::isListening() const
{
    return m_server && m_server->isListening();
}

quint16 FakeWssServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

QUrl FakeWssServer::liveUrl(const QString &host) const
{
    return QUrl(QStringLiteral("wss://%1:%2/live").arg(host).arg(port()));
}

// ---------------------------------------------------------------------------

FakeHttpsServer::FakeHttpsServer(const QSslConfiguration &config)
{
    m_server = new QSslServer;
    m_server->setSslConfiguration(config);
    m_server->setHandshakeTimeout(5000);
    m_server->listen(QHostAddress::LocalHost, 0);

    QObject::connect(m_server, &QTcpServer::pendingConnectionAvailable, m_server, [this] {
        auto *socket = qobject_cast<QSslSocket *>(m_server->nextPendingConnection());
        if (!socket)
            return;
        m_buffers.insert(socket, QByteArray());
        QObject::connect(socket, &QIODevice::readyRead, socket,
                         [this, socket] { onReadyRead(socket); });
        QObject::connect(socket, &QObject::destroyed, m_server,
                         [this, socket] { m_buffers.remove(socket); });
    });
}

FakeHttpsServer::~FakeHttpsServer()
{
    if (m_server) {
        m_server->close();
        delete m_server;
    }
}

bool FakeHttpsServer::isListening() const
{
    return m_server && m_server->isListening();
}

quint16 FakeHttpsServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

QUrl FakeHttpsServer::url(const QString &path, const QString &host) const
{
    return QUrl(QStringLiteral("https://%1:%2%3").arg(host).arg(port()).arg(path));
}

void FakeHttpsServer::enqueue(const QString &path, Response response)
{
    m_responses[path].enqueue(response);
}

int FakeHttpsServer::requestCount(const QString &path) const
{
    return m_requestCounts.value(path, 0);
}

QByteArray FakeHttpsServer::lastAuthorization(const QString &path) const
{
    return m_lastAuthorization.value(path);
}

QByteArray FakeHttpsServer::lastBody(const QString &path) const
{
    return m_lastBody.value(path);
}

QByteArray FakeHttpsServer::lastTarget(const QString &path) const
{
    return m_lastTarget.value(path);
}

void FakeHttpsServer::onReadyRead(QSslSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();

    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;

    const QByteArray headerBlock = buffer.left(headerEnd);
    const int firstLineEnd = headerBlock.indexOf("\r\n");
    const QByteArray requestLine = firstLineEnd < 0 ? headerBlock : headerBlock.left(firstLineEnd);
    const QByteArray headers = firstLineEnd < 0 ? QByteArray() : headerBlock.mid(firstLineEnd + 2);

    // Determine declared body length for POSTs.
    qint64 contentLength = 0;
    for (const QByteArray &line : headers.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("content-length:"))
            contentLength = trimmed.mid(QByteArrayLiteral("content-length:").size()).trimmed().toLongLong();
    }

    const int bodyStart = headerEnd + 4;
    if (buffer.size() - bodyStart < contentLength)
        return; // wait for the full body

    const QByteArray body = buffer.mid(bodyStart, static_cast<int>(contentLength));
    buffer.clear();
    dispatch(socket, requestLine, headers, body);
}

void FakeHttpsServer::dispatch(QSslSocket *socket, const QByteArray &requestLine,
                               const QByteArray &headers, const QByteArray &body)
{
    const QList<QByteArray> parts = requestLine.split(' ');
    QString path;
    QByteArray fullTarget;
    if (parts.size() >= 2) {
        fullTarget = parts.at(1);
        QByteArray target = fullTarget;
        const int q = target.indexOf('?');
        if (q >= 0)
            target = target.left(q);
        path = QString::fromLatin1(target);
    }

    QByteArray authorization;
    for (const QByteArray &line : headers.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("authorization:"))
            authorization = trimmed.mid(QByteArrayLiteral("authorization:").size()).trimmed();
    }

    m_requestCounts[path] += 1;
    m_lastAuthorization[path] = authorization;
    m_lastBody[path] = body;
    m_lastTarget[path] = fullTarget;

    Response response;
    if (m_responses.contains(path) && !m_responses[path].isEmpty())
        response = m_responses[path].dequeue();
    else
        response.status = 404;

    const qint64 declared = response.declaredLength >= 0 ? response.declaredLength
                                                         : response.body.size();

    QByteArray reply;
    reply += "HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n";
    reply += "Content-Type: application/cbor\r\n";
    reply += "Content-Length: " + QByteArray::number(declared) + "\r\n";
    reply += "Connection: close\r\n\r\n";
    reply += response.body;

    socket->write(reply);
    socket->flush();
    socket->disconnectFromHost();
}

} // namespace RelayTest
