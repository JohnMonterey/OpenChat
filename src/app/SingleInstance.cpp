#include "app/SingleInstance.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>

#ifdef Q_OS_WIN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

namespace OpenChat {
namespace {

constexpr QByteArrayView showRequest = "show\n";
constexpr int connectTimeoutMs = 1000;
constexpr int writeTimeoutMs = 1000;
// A request is one short line; anything longer is not from OpenChat.
constexpr qint64 maximumRequestBytes = 64;

// Local socket names are global to the machine (a named pipe on Windows, a
// socket in the temporary directory elsewhere), so the name is derived from
// the per-user directory that holds the lock.
QString serverNameFor(const QString &directory)
{
    const QByteArray digest = QCryptographicHash::hash(
        QDir(directory).absolutePath().toUtf8(), QCryptographicHash::Sha256);
    return QStringLiteral("OpenChat-") + QString::fromLatin1(digest.toHex().left(24));
}

} // namespace

SingleInstance::SingleInstance(const QString &directory, QObject *parent)
    : QObject(parent)
    , m_serverName(serverNameFor(directory))
    , m_lock(QDir(directory).filePath(QStringLiteral("instance.lock")))
{
    QDir().mkpath(directory);
    // Never stale by age: an OpenChat can run for weeks. A holder that died
    // is still recognised by its process id and taken over.
    m_lock.setStaleLockTime(0);
}

SingleInstance::~SingleInstance()
{
    release();
}

SingleInstance::Claim SingleInstance::claim(std::chrono::milliseconds waitForPrevious)
{
    if (m_lock.tryLock(0)) {
        listen();
        return Claim::Primary;
    }
    // A lock that cannot be taken for any other reason (a read-only or full
    // disk) says nothing about another OpenChat; refusing to start over it
    // would be worse than the problem it guards against.
    if (m_lock.error() != QLockFile::LockFailedError)
        return Claim::Primary;
    if (askRunningInstanceToShowItself())
        return Claim::HandedOver;
    // Held, but nobody answering: the previous OpenChat is closing (it stops
    // answering first), or has only just started and is not listening yet.
    if (m_lock.tryLock(int(waitForPrevious.count()))) {
        listen();
        return Claim::Primary;
    }
    if (askRunningInstanceToShowItself())
        return Claim::HandedOver;
    return Claim::StillRunning;
}

bool SingleInstance::askRunningInstanceToShowItself()
{
    QLocalSocket socket;
    socket.connectToServer(m_serverName);
    if (!socket.waitForConnected(connectTimeoutMs))
        return false;
#ifdef Q_OS_WIN
    // Windows lets only the foreground process bring a window to the front.
    // This launch was just started by the user, so it is foreground and can
    // pass that on; otherwise the running OpenChat could only flash its
    // taskbar button.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    socket.write(showRequest.data(), showRequest.size());
    if (!socket.waitForBytesWritten(writeTimeoutMs))
        return false;
    // No answer is awaited: a running OpenChat that is busy (still starting,
    // say) reads the request when it gets to it, and starting a second one in
    // the meantime is exactly what this avoids.
    socket.disconnectFromServer();
    return true;
}

void SingleInstance::listen()
{
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_serverName)) {
        // A socket file left behind by an OpenChat that crashed. Nobody else
        // can own it while this process holds the lock.
        QLocalServer::removeServer(m_serverName);
        if (!m_server->listen(m_serverName)) {
            // Still starts; a later launch then waits for the lock instead
            // of handing over.
            delete m_server;
            m_server = nullptr;
            return;
        }
    }
    connect(m_server, &QLocalServer::newConnection, this, &SingleInstance::readRequest);
}

void SingleInstance::readRequest()
{
    while (m_server != nullptr && m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        auto answer = [this, socket] {
            if (socket->canReadLine()) {
                if (socket->readLine(maximumRequestBytes).trimmed() == "show")
                    emit activationRequested();
                socket->disconnectFromServer();
            } else if (socket->bytesAvailable() > maximumRequestBytes) {
                socket->abort();
            }
        };
        connect(socket, &QLocalSocket::readyRead, this, answer);
        // The request may have arrived with the connection.
        answer();
    }
}

void SingleInstance::stopAnswering()
{
    if (m_server == nullptr)
        return;
    m_server->close();
    delete m_server;
    m_server = nullptr;
}

void SingleInstance::release()
{
    stopAnswering();
    if (m_lock.isLocked())
        m_lock.unlock();
}

} // namespace OpenChat
