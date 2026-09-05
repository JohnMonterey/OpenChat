#pragma once

#include "call/CallEngine.h"
#include "call/CallTypes.h"

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace OpenChat {

class ChatController;

// The QML-facing bridge for voice calls.
//
// Dual-world like the other controllers: constructed with no engine it drives a
// scripted preview the --call capture path renders, so the approved call screen
// can be reviewed without a peer, a microphone, or a network. setLiveEngine()
// swaps in the real CallEngine and every property below starts reflecting an
// actual call. The preview seam is never touched once live.
class CallController final : public QObject
{
    Q_OBJECT
    // The lifecycle is exposed to QML as the handful of booleans the view
    // actually branches on rather than as the enum itself: the view never needs
    // to distinguish Dialing from Connecting (both say "not yet talking"), and
    // keeping the enum C++-side means the states can be refined without
    // rewriting bindings.
    //
    // True whenever a call occupies the conversation surface. This is what
    // replaces the conversation header with the call screen.
    Q_PROPERTY(bool inCall READ inCall NOTIFY callChanged)
    Q_PROPERTY(bool isIncoming READ isIncoming NOTIFY callChanged)
    // True only while an incoming call is still waiting to be answered, i.e.
    // when the accept/decline pair should be offered.
    Q_PROPERTY(bool isRinging READ isRinging NOTIFY callChanged)
    Q_PROPERTY(bool isActive READ isActive NOTIFY callChanged)
    // True once the call is over and the surface is only reporting why.
    Q_PROPERTY(bool callEnded READ callEnded NOTIFY callChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY callChanged)
    Q_PROPERTY(QString peerName READ peerName NOTIFY callChanged)
    Q_PROPERTY(QString peerAvatarKey READ peerAvatarKey NOTIFY callChanged)
    Q_PROPERTY(QString localName READ localName NOTIFY callChanged)
    Q_PROPERTY(QString localAvatarKey READ localAvatarKey NOTIFY callChanged)
    Q_PROPERTY(bool localSpeaking READ localSpeaking NOTIFY levelsChanged)
    Q_PROPERTY(bool remoteSpeaking READ remoteSpeaking NOTIFY levelsChanged)
    Q_PROPERTY(double localLevel READ localLevel NOTIFY levelsChanged)
    Q_PROPERTY(double remoteLevel READ remoteLevel NOTIFY levelsChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY callChanged)
    Q_PROPERTY(QString durationText READ durationText NOTIFY durationChanged)
    // True when this build/machine can actually place a call, so the header's
    // call button can explain itself instead of failing silently.
    Q_PROPERTY(bool callsAvailable READ callsAvailable NOTIFY callChanged)

public:
    explicit CallController(QObject *parent = nullptr);
    ~CallController() override;

    [[nodiscard]] CallState callState() const noexcept { return m_state; }
    [[nodiscard]] bool inCall() const noexcept;
    [[nodiscard]] bool isIncoming() const noexcept;
    [[nodiscard]] bool isRinging() const noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] bool callEnded() const noexcept { return m_state == CallState::Ended; }
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString peerName() const { return m_peerName; }
    [[nodiscard]] QString peerAvatarKey() const { return m_peerAvatarKey; }
    [[nodiscard]] QString localName() const { return m_localName; }
    [[nodiscard]] QString localAvatarKey() const { return m_localAvatarKey; }
    [[nodiscard]] bool localSpeaking() const noexcept { return m_localSpeaking; }
    [[nodiscard]] bool remoteSpeaking() const noexcept { return m_remoteSpeaking; }
    [[nodiscard]] double localLevel() const noexcept { return m_localLevel; }
    [[nodiscard]] double remoteLevel() const noexcept { return m_remoteLevel; }
    [[nodiscard]] bool muted() const noexcept { return m_muted; }
    [[nodiscard]] QString durationText() const;
    [[nodiscard]] bool callsAvailable() const noexcept { return m_engine != nullptr; }

    // Calls whoever is open in the conversation pane. No-op when nothing is
    // open, when the contact has no reachable device, or when a call is running.
    Q_INVOKABLE void callCurrentContact();
    Q_INVOKABLE void callContact(const QString &contactId);
    Q_INVOKABLE void acceptCall();
    Q_INVOKABLE void declineCall();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void dismissCall();

    // How this user is shown on the call screen. Set from the profile in a live
    // session and directly in previews; a live session refreshes it whenever the
    // display name changes.
    void setLocalIdentity(const QString &name, const QString &avatarKey);

    // Live seam. Both are borrowed and must outlive this controller.
    void setLiveEngine(CallEngine *engine, ChatController *chats);

    // Preview seam for the --call capture path: pins the call screen into a
    // chosen state with scripted names and speaking indicators. Never reachable
    // from a live session.
    void enableForPreview(CallState state, const QString &peerName,
                          const QString &peerAvatarKey, bool remoteSpeaking,
                          bool localSpeaking);

signals:
    void callChanged();
    void levelsChanged();
    void durationChanged();
    // Raised when a call arrives, so the app can alert the user.
    void incomingCall();

private:
    void syncFromEngine();
    void syncLevels();

    CallEngine *m_engine = nullptr;
    ChatController *m_chats = nullptr;
    QTimer *m_durationTimer = nullptr;

    CallState m_state = CallState::Idle;
    CallEndReason m_endReason = CallEndReason::None;
    CallDirection m_direction = CallDirection::Outgoing;
    QString m_peerName;
    QString m_peerAvatarKey = QStringLiteral("userpfp_none");
    QString m_localName;
    QString m_localAvatarKey = QStringLiteral("userpfp_none");
    bool m_localSpeaking = false;
    bool m_remoteSpeaking = false;
    double m_localLevel = 0.0;
    double m_remoteLevel = 0.0;
    bool m_muted = false;
    qint64 m_durationMs = 0;
};

} // namespace OpenChat
