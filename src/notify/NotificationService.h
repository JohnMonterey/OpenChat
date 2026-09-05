#pragma once

#include "notify/Notification.h"
#include "notify/NotificationBackend.h"

#include <QObject>
#include <QSet>
#include <QString>

#include <memory>

namespace OpenChat {

// Decides which desktop notifications are worth showing, and what they say.
//
// Everything platform specific lives behind NotificationBackend; everything
// that is a judgement call lives here, so it is decided once and tested once:
//
//  - a message is not announced while the user is already looking at that
//    conversation in a focused window;
//  - opening a conversation takes back the notification that was about it;
//  - a second message from the same contact replaces the first rather than
//    stacking, so one talkative contact cannot bury the desktop;
//  - a burst from many contacts is capped, after which a single summary stands
//    in for the rest until the burst subsides;
//  - titles and bodies are elided before they reach the desktop, which is a
//    different trust domain from the encrypted store they came out of.
class NotificationService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)

public:
    // The sender's name is a display name, so it is bounded but not tiny; the
    // body is cut well short of any desktop's own limit so the elision is ours
    // and looks deliberate.
    static constexpr int maxTitleLength = 64;
    static constexpr int maxBodyLength = 220;
    // The picture is rendered once at a size every desktop downscales cleanly.
    static constexpr int avatarPixels = 96;
    // At most this many separate conversations are announced within
    // burstWindowMs; further ones collapse into one summary notification.
    static constexpr int burstLimit = 4;
    static constexpr int burstWindowMs = 5000;

    // Takes ownership of the backend, which must not be null. Pass a fake here
    // to test the policy without a desktop.
    explicit NotificationService(std::unique_ptr<NotificationBackend> backend,
                                 QObject *parent = nullptr);
    ~NotificationService() override;

    // Whether a post right now would reach the user: notifications turned on
    // and a working desktop service behind them.
    [[nodiscard]] bool isAvailable() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] bool isEnabled() const { return m_enabled; }
    // Turning notifications off also takes back anything still on screen.
    void setEnabled(bool enabled);

    // The application's window has, or has lost, keyboard focus.
    void setWindowActive(bool active);
    // The conversation the window is showing, or an empty string for none.
    // Setting it takes back any notification about that conversation, because
    // the user has now seen the message.
    void setActiveConversation(const QString &key);

    [[nodiscard]] bool windowActive() const { return m_windowActive; }
    [[nodiscard]] QString activeConversation() const { return m_activeConversation; }

    // Announces an inbound message. `conversationKey` identifies the chat (and
    // is what conversationActivated reports back), `senderName` and `body` are
    // shown as written apart from elision, and `avatarKey` is resolved through
    // the same artwork the interface draws. A blank body is reported as a
    // message having arrived rather than as an empty notification, which is
    // what the caller wants when the session state withholds plaintext.
    void postMessage(const QString &conversationKey, const QString &senderName,
                     const QString &body, const QString &avatarKey);

    // Posts an already-built notification, applying the same policy. Used by
    // postMessage and available for the other categories.
    void post(const Notification &notification);

    // Takes back everything on screen. Called on shutdown.
    void withdrawAll();

signals:
    void enabledChanged();
    // The user clicked a notification and wants this conversation opened.
    void conversationActivated(const QString &key);

private:
    // True when the message would be redundant: its conversation is on screen
    // in a window the user is looking at.
    [[nodiscard]] bool isRedundant(const QString &conversationKey) const;
    // Records this post against the burst budget and says whether it fits.
    // Resets the budget once burstWindowMs has passed with no post.
    [[nodiscard]] bool admitToBurst(const QString &conversationKey);
    [[nodiscard]] static QString elide(const QString &text, int limit);

    std::unique_ptr<NotificationBackend> m_backend;
    bool m_enabled = true;
    bool m_windowActive = false;
    QString m_activeConversation;

    // Keys currently posted, so withdrawAll and setActiveConversation know what
    // there is to take back.
    QSet<QString> m_shown;
    // Burst accounting: how many conversations have been announced since the
    // window opened, when it opened, and what the collapsed ones were about.
    int m_burstCount = 0;
    qint64 m_burstStartedMs = 0;
    int m_burstSuppressed = 0;
    QString m_burstLatestKey;
};

} // namespace OpenChat
