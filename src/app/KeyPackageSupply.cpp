#include "app/KeyPackageSupply.h"
#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "network/RelayClient.h"
#include "diagnostics/Logging.h"

namespace OpenChat {
KeyPackageSupply::KeyPackageSupply(ProfileSession &session, RelayClient &relay, QObject *parent)
    : QObject(parent), m_session(session), m_relay(relay)
{
    m_checkTimer.setInterval(60 * 60 * 1000); // TTL can expire while continuously online
    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(30'000);
    connect(&m_checkTimer, &QTimer::timeout, this, &KeyPackageSupply::check);
    connect(&m_retryTimer, &QTimer::timeout, this, &KeyPackageSupply::check);
    connect(&relay, &RelayClient::connected, this, &KeyPackageSupply::check);
    connect(&relay, &RelayClient::keyPackageCountReceived, this, &KeyPackageSupply::receivedCount);
    connect(&relay, &RelayClient::keyPackageCountFailed, this, [this] {
        m_checking = false;
        retry();
    });
    connect(&relay, &RelayClient::keyPackagePublished, this, [this] {
        if (!m_publishing)
            return;
        m_publishing = false;
        if (!m_publishCountReceived && m_available >= 0)
            ++m_available;
        // Queue the next generation so unrelated signal subscribers finish first.
        QTimer::singleShot(0, this, &KeyPackageSupply::publishNext);
    });
    connect(&relay, &RelayClient::keyPackagePublishFailed, this, [this] {
        if (!m_publishing)
            return;
        m_publishing = false;
        m_available = -1; // a lost response may still have stored the package
        qCWarning(relayLog) << "Key package publish failed; retrying supply check";
        retry();
    });
}

void KeyPackageSupply::start(int available)
{
    if (m_running)
        return;
    m_running = true;
    m_checkTimer.start();
    if (available >= 0)
        receivedCount(available);
    else
        check();
}

void KeyPackageSupply::pause()
{
    m_running = false;
    m_checking = false;
    m_publishing = false;
    m_filling = false;
    m_available = -1;
    m_checkTimer.stop();
    m_retryTimer.stop();
}

void KeyPackageSupply::retry()
{
    if (m_running && !m_retryTimer.isActive())
        m_retryTimer.start();
}

void KeyPackageSupply::check()
{
    if (!m_running || !m_session.isUnlocked() || m_publishing || m_checking)
        return;
    m_checking = true;
    m_relay.fetchKeyPackageCount();
}

void KeyPackageSupply::receivedCount(int available)
{
    if (!m_running || available < 0)
        return;
    m_checking = false;
    m_retryTimer.stop();
    m_available = available;
    qCDebug(relayLog) << "Available key packages:" << available;
    if (m_publishing) {
        m_publishCountReceived = true;
        return;
    }
    if (available < minimumAvailable)
        m_filling = true;
    QTimer::singleShot(0, this, &KeyPackageSupply::publishNext);
}

void KeyPackageSupply::publishNext()
{
    if (!m_running || !m_session.isUnlocked() || m_publishing || m_checking
        || !m_filling || m_available < 0 || m_retryTimer.isActive())
        return;
    if (m_available >= targetAvailable) {
        m_filling = false;
        return;
    }
    auto *mls = m_session.mls();
    if (!mls)
        return;
    const auto package = mls->generateKeyPackage();
    if (!package.hasValue() || !m_session.persistMlsState().hasValue()) {
        qCWarning(mlsLog) << "Cannot persist new key package; nothing published";
        retry();
        return;
    }
    // The private material is durable BEFORE the relay can expose this package.
    m_publishing = true;
    m_publishCountReceived = false;
    m_relay.publishKeyPackage(package.value());
}
} // namespace OpenChat
