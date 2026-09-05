#include "controllers/CallController.h"

#include "controllers/ChatController.h"

#include <QTimer>

namespace OpenChat {

namespace {

// The call duration only ever changes at second resolution, so refreshing it
// once a second is exactly enough.
constexpr int durationRefreshMs = 1000;

[[nodiscard]] QString formatDuration(qint64 milliseconds)
{
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    // Hours are spelled out rather than rolled into minutes, so a long call does
    // not read as "83:14".
    if (minutes >= 60) {
        return QStringLiteral("%1:%2:%3")
            .arg(minutes / 60)
            .arg(minutes % 60, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace

CallController::CallController(QObject *parent)
    : QObject(parent)
{
    m_durationTimer = new QTimer(this);
    m_durationTimer->setInterval(durationRefreshMs);
    connect(m_durationTimer, &QTimer::timeout, this, [this] {
        if (m_engine == nullptr)
            return;
        const qint64 elapsed = m_engine->activeDurationMs();
        if (elapsed / 1000 == m_durationMs / 1000)
            return;
        m_durationMs = elapsed;
        emit durationChanged();
    });
}

CallController::~CallController() = default;

bool CallController::inCall() const noexcept
{
    // Ended is included deliberately: the call screen stays up long enough to
    // say why the call finished rather than snapping back to the conversation.
    return m_state != CallState::Idle;
}

bool CallController::isIncoming() const noexcept
{
    return m_direction == CallDirection::Incoming;
}

bool CallController::isRinging() const noexcept
{
    return m_state == CallState::Ringing && m_direction == CallDirection::Incoming;
}

bool CallController::isActive() const noexcept
{
    return m_state == CallState::Active;
}

QString CallController::statusText() const
{
    switch (m_state) {
    case CallState::Idle:
        return QString();
    case CallState::Dialing:
        return QStringLiteral("Calling…");
    case CallState::Ringing:
        return m_direction == CallDirection::Incoming ? QStringLiteral("Incoming call")
                                                      : QStringLiteral("Ringing…");
    case CallState::Connecting:
        return QStringLiteral("Connecting…");
    case CallState::Active:
        return durationText();
    case CallState::Ended:
        return callEndReasonName(m_endReason);
    }
    return QString();
}

QString CallController::durationText() const
{
    return formatDuration(m_durationMs);
}

void CallController::callCurrentContact()
{
    if (m_chats == nullptr)
        return;
    callContact(m_chats->currentContactId());
}

void CallController::callContact(const QString &contactId)
{
    if (m_engine == nullptr || m_chats == nullptr || contactId.isEmpty())
        return;
    const std::optional<ChatController::CallRoute> route = m_chats->callRouteFor(contactId);
    if (!route)
        return;

    CallEngine::CallPeer peer;
    peer.conversation = route->conversation;
    peer.device = route->device;
    peer.contactId = route->contactId;
    peer.displayName = route->displayName;
    peer.avatarKey = route->avatarKey;
    (void)m_engine->placeCall(peer);
}

void CallController::acceptCall()
{
    if (m_engine != nullptr)
        m_engine->acceptCall();
}

void CallController::declineCall()
{
    if (m_engine != nullptr)
        m_engine->declineCall();
}

void CallController::hangUp()
{
    if (m_engine != nullptr)
        m_engine->hangUp();
}

void CallController::toggleMute()
{
    if (m_engine != nullptr)
        m_engine->setMuted(!m_engine->isMuted());
}

void CallController::dismissCall()
{
    if (m_engine != nullptr)
        m_engine->dismissEndedCall();
}

void CallController::setLocalIdentity(const QString &name, const QString &avatarKey)
{
    if (name == m_localName && avatarKey == m_localAvatarKey)
        return;
    m_localName = name;
    if (!avatarKey.isEmpty())
        m_localAvatarKey = avatarKey;
    emit callChanged();
}

void CallController::setLiveEngine(CallEngine *engine, ChatController *chats)
{
    if (engine == nullptr || chats == nullptr)
        return;
    m_engine = engine;
    m_chats = chats;
    setLocalIdentity(chats->localUserName(), chats->localAvatarKey());

    connect(engine, &CallEngine::stateChanged, this, &CallController::syncFromEngine);
    connect(engine, &CallEngine::mutedChanged, this, &CallController::syncFromEngine);
    connect(engine, &CallEngine::levelsChanged, this, &CallController::syncLevels);
    connect(engine, &CallEngine::incomingCall, this, &CallController::incomingCall);
    connect(chats->contacts(), &QAbstractItemModel::modelReset,
            this, &CallController::syncFromEngine);
    connect(chats, &ChatController::localUserNameChanged, this, [this] {
        setLocalIdentity(m_chats->localUserName(), m_chats->localAvatarKey());
    });
    syncFromEngine();
}

void CallController::syncFromEngine()
{
    if (m_engine == nullptr)
        return;
    m_state = m_engine->state();
    m_endReason = m_engine->endReason();
    m_direction = m_engine->direction();
    m_muted = m_engine->isMuted();

    const CallEngine::CallPeer &peer = m_engine->peer();
    // Incoming offers carry routing IDs, not display names. Use the same saved
    // identity as the chat roster, including handles resolved while ringing.
    const auto route = m_chats->callRouteFor(peer.conversation, peer.device);
    const QString name = route ? route->displayName : peer.displayName;
    const QString avatarKey = route ? route->avatarKey : peer.avatarKey;
    m_peerName = name.isEmpty() ? QStringLiteral("Unknown caller") : name;
    m_peerAvatarKey = avatarKey.isEmpty() ? QStringLiteral("userpfp_none") : avatarKey;

    if (m_state == CallState::Active) {
        m_durationTimer->start();
    } else {
        m_durationTimer->stop();
        // Freeze the duration on Ended so the summary keeps the final length,
        // and clear it once the call surface is dismissed.
        if (m_state == CallState::Idle)
            m_durationMs = 0;
    }
    if (m_state != CallState::Active) {
        m_localSpeaking = false;
        m_remoteSpeaking = false;
        m_localLevel = 0.0;
        m_remoteLevel = 0.0;
        emit levelsChanged();
    }
    emit callChanged();
    emit durationChanged();
}

void CallController::syncLevels()
{
    if (m_engine == nullptr)
        return;
    const bool localSpeaking = m_engine->isLocalSpeaking();
    const bool remoteSpeaking = m_engine->isRemoteSpeaking();
    const double localLevel = m_engine->localLevel();
    const double remoteLevel = m_engine->remoteLevel();
    if (localSpeaking == m_localSpeaking && remoteSpeaking == m_remoteSpeaking
        && qFuzzyCompare(localLevel + 1.0, m_localLevel + 1.0)
        && qFuzzyCompare(remoteLevel + 1.0, m_remoteLevel + 1.0))
        return;
    m_localSpeaking = localSpeaking;
    m_remoteSpeaking = remoteSpeaking;
    m_localLevel = localLevel;
    m_remoteLevel = remoteLevel;
    emit levelsChanged();

    if (m_engine->state() == CallState::Active) {
        const qint64 elapsed = m_engine->activeDurationMs();
        if (elapsed / 1000 != m_durationMs / 1000) {
            m_durationMs = elapsed;
            emit durationChanged();
        }
    }
}

void CallController::enableForPreview(CallState state, const QString &peerName,
                                      const QString &peerAvatarKey, bool remoteSpeaking,
                                      bool localSpeaking)
{
    if (m_engine != nullptr)
        return; // a live controller is never overwritten by preview state
    m_state = state;
    m_direction = state == CallState::Ringing ? CallDirection::Incoming : CallDirection::Outgoing;
    m_peerName = peerName;
    m_peerAvatarKey = peerAvatarKey;
    m_remoteSpeaking = remoteSpeaking;
    m_localSpeaking = localSpeaking;
    m_remoteLevel = remoteSpeaking ? 0.42 : 0.0;
    m_localLevel = localSpeaking ? 0.31 : 0.0;
    m_durationMs = state == CallState::Active ? 154'000 : 0;
    emit callChanged();
    emit levelsChanged();
    emit durationChanged();
}

} // namespace OpenChat
