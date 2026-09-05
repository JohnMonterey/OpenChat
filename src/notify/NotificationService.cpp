#include "notify/NotificationService.h"

#include "render/AvatarPainter.h"

#include <QDateTime>

#include <utility>

namespace OpenChat {

namespace {

// The key a collapsed burst is posted under. It cannot collide with a
// conversation key, which is an account id in hex.
const QString burstSummaryKey = QStringLiteral("openchat:burst-summary");

// The picture is drawn with the same corner rounding the interface uses: the
// sidebar draws a 5 px radius on a 40 px avatar, so the ratio carries over to
// whatever size the notification is rendered at.
constexpr qreal avatarCornerRatio = 0.125;

} // namespace

NotificationService::NotificationService(std::unique_ptr<NotificationBackend> backend,
                                         QObject *parent)
    : QObject(parent), m_backend(std::move(backend))
{
    Q_ASSERT(m_backend);
    connect(m_backend.get(), &NotificationBackend::activated, this,
            [this](const QString &key) {
                m_shown.remove(key);
                // A collapsed burst has no conversation of its own; clicking it
                // opens the most recent chat it stood in for.
                if (key == burstSummaryKey) {
                    if (!m_burstLatestKey.isEmpty())
                        emit conversationActivated(m_burstLatestKey);
                    return;
                }
                emit conversationActivated(key);
            });
}

NotificationService::~NotificationService()
{
    // A closed application should leave nothing on the desktop behind it.
    if (m_backend)
        m_backend->withdrawAll();
}

bool NotificationService::isAvailable() const
{
    return m_enabled && m_backend->isAvailable();
}

QString NotificationService::backendName() const
{
    return m_backend->name();
}

void NotificationService::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!m_enabled)
        withdrawAll();
    emit enabledChanged();
}

void NotificationService::setWindowActive(bool active)
{
    if (m_windowActive == active)
        return;
    m_windowActive = active;
    // Focusing the window on a conversation means its notification has served
    // its purpose.
    if (m_windowActive && !m_activeConversation.isEmpty() && m_shown.contains(m_activeConversation)) {
        m_backend->withdraw(m_activeConversation);
        m_shown.remove(m_activeConversation);
    }
}

void NotificationService::setActiveConversation(const QString &key)
{
    m_activeConversation = key;
    if (key.isEmpty() || !m_shown.contains(key))
        return;
    m_backend->withdraw(key);
    m_shown.remove(key);
}

bool NotificationService::isRedundant(const QString &conversationKey) const
{
    return m_windowActive && !conversationKey.isEmpty()
        && conversationKey == m_activeConversation;
}

QString NotificationService::elide(const QString &text, int limit)
{
    const QString collapsed = text.simplified();
    if (collapsed.size() <= limit)
        return collapsed;
    // Cut on a word boundary when one is close enough that the result still
    // reads as a sentence rather than a truncated word.
    int cut = limit - 1;
    const int lastSpace = collapsed.lastIndexOf(QLatin1Char(' '), cut);
    if (lastSpace > limit / 2)
        cut = lastSpace;
    return collapsed.left(cut).trimmed() + QStringLiteral("…");
}

bool NotificationService::admitToBurst(const QString &conversationKey)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_burstStartedMs == 0 || now - m_burstStartedMs > burstWindowMs) {
        m_burstStartedMs = now;
        m_burstCount = 0;
        m_burstSuppressed = 0;
        m_burstLatestKey.clear();
    }
    // Replacing a notification the user can already see is not new noise, so it
    // never counts against the budget.
    if (m_shown.contains(conversationKey))
        return true;
    if (m_burstCount < burstLimit) {
        ++m_burstCount;
        return true;
    }
    ++m_burstSuppressed;
    m_burstLatestKey = conversationKey;
    return false;
}

void NotificationService::post(const Notification &notification)
{
    if (!isAvailable())
        return;
    if (isRedundant(notification.key))
        return;

    if (!admitToBurst(notification.key)) {
        // Too many conversations at once: stand one summary in for the rest,
        // replacing itself as further messages arrive.
        Notification summary;
        summary.key = burstSummaryKey;
        summary.category = notification.category;
        summary.title = QStringLiteral("New messages");
        summary.body = m_burstSuppressed == 1
            ? QStringLiteral("1 more message from another chat")
            : QStringLiteral("%1 more messages from other chats").arg(m_burstSuppressed);
        m_backend->show(summary);
        m_shown.insert(summary.key);
        return;
    }

    Notification bounded = notification;
    bounded.title = elide(bounded.title, maxTitleLength);
    bounded.body = elide(bounded.body, maxBodyLength);
    m_backend->show(bounded);
    m_shown.insert(bounded.key);
}

void NotificationService::postMessage(const QString &conversationKey, const QString &senderName,
                                      const QString &body, const QString &avatarKey)
{
    if (!isAvailable() || isRedundant(conversationKey))
        return; // nothing to render an avatar for

    Notification notification;
    notification.key = conversationKey;
    notification.category = NotificationCategory::Message;
    notification.title =
        senderName.trimmed().isEmpty() ? QStringLiteral("New message") : senderName.trimmed();
    notification.body =
        body.trimmed().isEmpty() ? QStringLiteral("Sent you a message") : body;
    notification.image =
        AvatarPainter::render(avatarKey, avatarPixels, avatarPixels * avatarCornerRatio);
    post(notification);
}

void NotificationService::withdrawAll()
{
    m_backend->withdrawAll();
    m_shown.clear();
    m_burstStartedMs = 0;
    m_burstCount = 0;
    m_burstSuppressed = 0;
    m_burstLatestKey.clear();
}

} // namespace OpenChat
