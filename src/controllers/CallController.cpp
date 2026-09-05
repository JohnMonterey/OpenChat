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
    connect(&m_camera, &QtVideoCapture::frameCaptured, this, [this](const QImage &image) {
        if (!m_cameraEnabled)
            return;
        const QSize oldSize = m_localVideo.size();
        m_localVideo = image;
        emit localVideoChanged();
        if (oldSize != image.size())
            emit videoChanged();
        if (m_engine)
            m_engine->sendVideoFrame(image);
    });
    connect(&m_camera, &QtVideoCapture::failed, this, [this](const QString &message) {
        stopCamera();
        m_cameraError = message;
        emit videoChanged();
    });
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
        return m_isGroupCall ? QStringLiteral("Calling the group…") : QStringLiteral("Calling…");
    case CallState::Ringing:
        if (m_isGroupCall)
            return m_direction == CallDirection::Incoming ? QStringLiteral("Incoming group call")
                                                          : QStringLiteral("Ringing everyone…");
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

void CallController::callCurrentContact(bool video)
{
    if (m_chats == nullptr)
        return;
    if (m_engine == nullptr || callOccupiesDevice(m_engine->state()))
        return;
    callContact(m_chats->currentContactId());
    if (video && callOccupiesDevice(m_engine->state()) && !isIncoming())
        toggleCamera();
}

void CallController::callContact(const QString &contactId)
{
    if (m_engine == nullptr || m_chats == nullptr || contactId.isEmpty())
        return;
    if (ChatController::isGroupChatId(contactId)) {
        if (const auto group = m_chats->groupCallRouteFor(contactId))
            (void)m_engine->placeGroupCall(*group);
        return;
    }
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

void CallController::toggleCamera()
{
    if (!inCall() || isRinging() || callEnded())
        return;
    m_cameraError.clear();
    if (m_cameraEnabled) {
        stopCamera();
    } else if (m_engine) {
        m_cameraEnabled = true;
        emit videoChanged();
        m_camera.start();
    }
}

void CallController::stopCamera()
{
    m_camera.stop();
    const bool wasEnabled = m_cameraEnabled;
    m_cameraEnabled = false;
    m_localVideo = QImage();
    if (wasEnabled && m_engine)
        m_engine->sendVideoFrame(QImage());
    emit localVideoChanged();
    emit videoChanged();
}

void CallController::setRemoteVideo(const QImage &image)
{
    const QSize oldSize = m_remoteVideo.size();
    m_remoteVideo = image;
    emit remoteVideoChanged();
    if (oldSize != image.size())
        emit videoChanged();
}

void CallController::setPreviewVideo(const QImage &local, const QImage &remote)
{
    if (m_engine)
        return;
    m_cameraEnabled = !local.isNull();
    m_localVideo = local;
    setRemoteVideo(remote);
    emit localVideoChanged();
    emit videoChanged();
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

    connect(engine, &CallEngine::remoteVideoFrame, this, &CallController::setRemoteVideo);
    connect(engine, &CallEngine::stateChanged, this, &CallController::syncFromEngine);
    connect(engine, &CallEngine::mutedChanged, this, &CallController::syncFromEngine);
    connect(engine, &CallEngine::levelsChanged, this, &CallController::syncLevels);
    connect(engine, &CallEngine::incomingCall, this, &CallController::incomingCall);
    connect(engine, &CallEngine::participantsChanged, this, &CallController::syncParticipants);
    connect(engine, &CallEngine::participantVideoFrame, this,
            [this](const DeviceId &device, const QImage &image) {
                m_participants.setVideoFrame(device.toHex(), image);
            });
    connect(chats->contacts(), &QAbstractItemModel::modelReset,
            this, &CallController::syncFromEngine);
    // An offer on a group conversation rings the group. The roster answers
    // which conversations are groups and who is in them.
    engine->groupRouteResolver = [chats](const ConversationId &conversation) {
        return chats->groupCallRouteFor(conversation);
    };
    connect(chats, &ChatController::localUserNameChanged, this, [this] {
        setLocalIdentity(m_chats->localUserName(), m_chats->localAvatarKey());
    });
    connect(chats, &ChatController::localProfileChanged, this, [this] {
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
    if (m_state == CallState::Idle || m_state == CallState::Ended || isRinging()) {
        stopCamera();
        setRemoteVideo(QImage());
        m_cameraError.clear();
        emit videoChanged();
    }

    const CallEngine::CallPeer &peer = m_engine->peer();
    m_isGroupCall = m_engine->isGroupCall();
    if (m_isGroupCall) {
        // The headline is the group; the members are the participant rows.
        const auto route = m_chats->groupCallRouteFor(peer.conversation);
        m_groupTitle = route ? route->title : m_engine->groupTitle();
        if (m_groupTitle.isEmpty())
            m_groupTitle = QStringLiteral("Group call");
        m_peerName = m_groupTitle;
        m_peerAvatarKey = QStringLiteral("group");
        syncParticipants();
    } else {
        m_groupTitle.clear();
        // Incoming offers carry routing IDs, not display names. Use the same saved
        // identity as the chat roster, including handles resolved while ringing.
        const auto route = m_chats->callRouteFor(peer.conversation, peer.device);
        const QString name = route ? route->displayName : peer.displayName;
        const QString avatarKey = route ? route->avatarKey : peer.avatarKey;
        m_peerName = name.isEmpty() ? QStringLiteral("Unknown caller") : name;
        m_peerAvatarKey = avatarKey.isEmpty() ? QStringLiteral("userpfp_none") : avatarKey;
        if (m_participants.count() > 0)
            m_participants.setParticipants({});
        m_joinedCount = 0;
    }

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

void CallController::syncParticipants()
{
    if (m_engine == nullptr)
        return;
    QVector<CallParticipantRow> rows;
    int joined = 0;
    for (const CallEngine::Participant &participant : m_engine->participants()) {
        CallParticipantRow row;
        row.deviceId = participant.peer.device.toHex();
        // The roster's current name for a member who is a contact beats the
        // one the call was placed with (a handle can resolve mid-call).
        const auto route = m_chats->callRouteFor(participant.peer.contactId);
        row.name = route ? route->displayName : participant.peer.displayName;
        if (row.name.isEmpty())
            row.name = QStringLiteral("Unknown");
        row.avatarKey = route ? route->avatarKey : participant.peer.avatarKey;
        if (row.avatarKey.isEmpty())
            row.avatarKey = QStringLiteral("userpfp_none");
        row.stateText = callParticipantStateName(participant.state);
        row.joined = participant.state == CallParticipantState::Joined;
        row.ringing = participant.state == CallParticipantState::Ringing;
        row.speaking = participant.speaking;
        row.level = participant.level;
        if (row.joined)
            ++joined;
        rows.append(row);
    }
    m_participants.setParticipants(std::move(rows));
    if (joined != m_joinedCount) {
        m_joinedCount = joined;
        emit callChanged();
    }
}

void CallController::enableForGroupPreview(CallState state, const QString &title,
                                           const QVector<CallParticipantRow> &participants)
{
    if (m_engine != nullptr)
        return; // a live controller is never overwritten by preview state
    m_state = state;
    m_direction = state == CallState::Ringing ? CallDirection::Incoming : CallDirection::Outgoing;
    m_isGroupCall = state != CallState::Idle;
    m_groupTitle = title;
    m_peerName = title;
    m_peerAvatarKey = QStringLiteral("group");
    m_joinedCount = 0;
    for (const CallParticipantRow &row : participants)
        if (row.joined)
            ++m_joinedCount;
    m_participants.setParticipants(participants);
    m_remoteSpeaking = false;
    m_localSpeaking = false;
    m_remoteLevel = 0.0;
    m_localLevel = 0.0;
    m_durationMs = state == CallState::Active ? 154'000 : 0;
    emit callChanged();
    emit levelsChanged();
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
    m_isGroupCall = false;
    m_groupTitle.clear();
    m_joinedCount = 0;
    if (m_participants.count() > 0)
        m_participants.setParticipants({});
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
