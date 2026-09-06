#include "call/CallEngine.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <limits>

namespace OpenChat {

namespace {

// How often the UI's level indicators are refreshed. 20 Hz is smooth to the eye
// and two orders of magnitude cheaper than emitting once per 20 ms audio frame.
constexpr int levelRefreshMs = 50;

// Slack added to the hang-up sound's own length before the speaker is closed.
// Enough to cover the device's own buffering; short enough that nothing is
// holding the audio device open in the background afterwards.
constexpr int playbackTailSlackMs = 250;

// A group member's camera whose frames stop arriving reads as off after this.
constexpr qint64 videoStaleMs = 2500;

// A share sends a heartbeat every second even when the desktop is motionless,
// so several missed in a row is a share that has genuinely gone rather than one
// whose sender simply stopped moving the mouse.
constexpr qint64 screenStaleMs = 4000;

// How often each receiving session is asked whether its periodic report is due.
// The report interval itself lives in the session; this only has to be finer.
constexpr int screenFeedbackPumpMs = 200;

// Sums `frame` into `mix` sample by sample, saturating rather than wrapping, so
// three people talking at once get loud instead of turning into noise.
void mixInto(AudioFrame &mix, const AudioFrame &frame)
{
    if (!isFullAudioFrame(mix) || !isFullAudioFrame(frame))
        return;
    auto *out = reinterpret_cast<qint16 *>(mix.data());
    const auto *in = reinterpret_cast<const qint16 *>(frame.constData());
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
        const int sum = int(out[i]) + int(in[i]);
        out[i] = static_cast<qint16>(std::clamp(sum, int(std::numeric_limits<qint16>::min()),
                                                int(std::numeric_limits<qint16>::max())));
    }
}

// The codec a pair of group members runs: the announced one when this build
// has it, otherwise PCM, which every build has. Both ends evaluate the same rule
// on each other's announcement, so they always agree.
[[nodiscard]] AudioCodecKind pairCodec(AudioCodecKind announced)
{
    return isAudioCodecAvailable(announced) ? announced : AudioCodecKind::Pcm;
}

} // namespace

qint64 steadyClockMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

QString callParticipantStateName(CallParticipantState state)
{
    switch (state) {
    case CallParticipantState::Ringing:
        return QStringLiteral("Ringing…");
    case CallParticipantState::Joined:
        return QString();
    case CallParticipantState::Declined:
        return QStringLiteral("Declined");
    case CallParticipantState::Left:
        return QStringLiteral("Left");
    case CallParticipantState::Busy:
        return QStringLiteral("Busy");
    case CallParticipantState::NoAnswer:
        return QStringLiteral("No answer");
    }
    return QString();
}

CallEngine::CallEngine(Config config, CallTransport &transport, CallAudioIoFactory audioIo,
                       QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_transport(transport)
    , m_audioIo(std::move(audioIo))
    , m_microphone(config.microphone)
{
    if (!isAudioCodecAvailable(m_config.preferredCodec))
        m_config.preferredCodec = AudioCodecKind::Pcm;

    m_videoTimeout = new QTimer(this);
    m_videoTimeout->setSingleShot(true);
    connect(m_videoTimeout, &QTimer::timeout, this, [this] { emit remoteVideoFrame(QImage()); });

    m_screenTimeout = new QTimer(this);
    m_screenTimeout->setSingleShot(true);
    connect(m_screenTimeout, &QTimer::timeout, this, [this] {
        if (m_remoteScreenActive) {
            m_remoteScreenActive = false;
            if (m_screenSession)
                m_screenSession->resetReceiver();
            emit remoteScreenFrame({});
        }
    });

    m_screenFeedbackTimer = new QTimer(this);
    m_screenFeedbackTimer->setInterval(screenFeedbackPumpMs);
    connect(m_screenFeedbackTimer, &QTimer::timeout, this, &CallEngine::pumpScreenFeedback);

    m_ringTimer = new QTimer(this);
    m_ringTimer->setSingleShot(true);
    connect(m_ringTimer, &QTimer::timeout, this, &CallEngine::onRingTimeout);

    m_stallTimer = new QTimer(this);
    m_stallTimer->setSingleShot(true);
    connect(m_stallTimer, &QTimer::timeout, this,
            [this] { endCall(CallEndReason::TransportFailed, /*notifyPeer=*/true); });

    m_levelTimer = new QTimer(this);
    m_levelTimer->setInterval(levelRefreshMs);
    connect(m_levelTimer, &QTimer::timeout, this, [this] {
        refreshParticipantLevels();
        emit levelsChanged();
    });

    m_playbackTailTimer = new QTimer(this);
    m_playbackTailTimer->setSingleShot(true);
    connect(m_playbackTailTimer, &QTimer::timeout, this, [this] {
        // Only let go of the speaker if no new call grabbed it in the meantime.
        if (!callOccupiesDevice(m_state))
            closePlayback();
    });

    m_transport.onSignal = [this](const ConversationId &conversation, const DeviceId &sender,
                                  const QByteArray &payload) {
        onSignal(conversation, sender, payload);
    };
    m_transport.onMedia = [this](const ConversationId &conversation, const DeviceId &sender,
                                 const QByteArray &packet) {
        onMedia(conversation, sender, packet);
    };
}

CallEngine::~CallEngine()
{
    // Drop the transport callbacks before the members they capture go away: the
    // engine is destroyed while the transport it borrows is still alive.
    m_transport.onSignal = nullptr;
    m_transport.onMedia = nullptr;
    m_sounds.stopAll();
    stopMedia();
}

qint64 CallEngine::activeDurationMs() const
{
    if (m_activeSinceMs == 0 || m_state != CallState::Active)
        return 0;
    return QDateTime::currentMSecsSinceEpoch() - m_activeSinceMs;
}

double CallEngine::localLevel() const
{
    if (m_session)
        return m_session->localLevel();
    if (m_group) {
        // Every member's session meters the same microphone; any one will do.
        for (const Member &member : m_group->members)
            if (member.session)
                return member.session->localLevel();
    }
    return 0.0;
}

double CallEngine::remoteLevel() const
{
    if (m_session)
        return m_session->remoteLevel();
    double loudest = 0.0;
    if (m_group)
        for (const Member &member : m_group->members)
            if (member.session)
                loudest = std::max(loudest, member.session->remoteLevel());
    return loudest;
}

bool CallEngine::isLocalSpeaking() const
{
    if (m_state != CallState::Active)
        return false;
    if (m_session)
        return m_session->isLocalSpeaking();
    if (m_group)
        for (const Member &member : m_group->members)
            if (member.session)
                return member.session->isLocalSpeaking();
    return false;
}

bool CallEngine::isRemoteSpeaking() const
{
    if (m_state != CallState::Active)
        return false;
    if (m_session)
        return m_session->isRemoteSpeaking();
    if (m_group)
        for (const Member &member : m_group->members)
            if (member.session && member.session->isRemoteSpeaking())
                return true;
    return false;
}

QString CallEngine::groupTitle() const
{
    return m_group ? m_group->title : QString();
}

QVector<CallEngine::Participant> CallEngine::participants() const
{
    QVector<Participant> result;
    if (!m_group)
        return result;
    result.reserve(static_cast<qsizetype>(m_group->members.size()));
    for (const Member &member : m_group->members)
        result.append(member.info);
    return result;
}

int CallEngine::joinedParticipantCount() const
{
    if (!m_group)
        return 0;
    return static_cast<int>(std::count_if(
        m_group->members.cbegin(), m_group->members.cend(),
        [](const Member &member) { return member.info.state == CallParticipantState::Joined; }));
}

const CallSession *CallEngine::sessionFor(const DeviceId &device) const
{
    if (!m_group)
        return m_peer.device == device ? m_session.get() : nullptr;
    for (const Member &member : m_group->members)
        if (member.info.peer.device == device)
            return member.session.get();
    return nullptr;
}

CallEngine::Member *CallEngine::memberFor(const DeviceId &device)
{
    if (!m_group)
        return nullptr;
    for (Member &member : m_group->members)
        if (member.info.peer.device == device)
            return &member;
    return nullptr;
}

bool CallEngine::placeCall(const CallPeer &peer)
{
    if (callOccupiesDevice(m_state))
        return false;
    const QByteArray secret = generateCallSecret();
    // No secret means no media key. Refusing here is the only safe outcome: a
    // call that proceeded without one would have to send audio unprotected.
    if (secret.size() != callSecretBytes)
        return false;

    m_group.reset();
    m_peer = peer;
    m_direction = CallDirection::Outgoing;
    m_callId = CallId::generate();
    m_secret = secret;
    m_codec = m_config.preferredCodec;
    m_endReason = CallEndReason::None;
    m_muted = false;

    // Bring the speaker up now, not on answer: the ringback has to be audible
    // for the whole wait, which is precisely the part of a call where there is
    // no media session yet.
    (void)openPlayback();
    if (m_soundsEnabled)
        m_sounds.startLoop(CallSound::Ringback);

    // The state is published BEFORE the offer goes out. Sending can deliver the
    // peer's reply synchronously (a loopback transport does exactly that, and
    // nothing in the transport contract forbids it), and a reply that arrived
    // while this engine still looked idle would be discarded as unsolicited.
    m_ringTimer->start(m_config.ringTimeoutMs);
    setState(CallState::Dialing);
    send(CallSignalMessage::offer(*m_callId, m_secret, m_codec));
    return true;
}

bool CallEngine::placeGroupCall(const GroupCallRoute &route)
{
    if (callOccupiesDevice(m_state) || route.members.isEmpty() || !m_config.localDevice)
        return false;
    const QByteArray secret = generateCallSecret();
    if (secret.size() != callSecretBytes)
        return false;

    auto group = std::make_unique<GroupCall>();
    group->conversation = route.conversation;
    group->title = route.title;
    for (const CallPeer &peer : route.members) {
        if (peer.device == *m_config.localDevice || memberFor(peer.device) != nullptr)
            continue;
        Member member;
        member.info.peer = peer;
        member.info.peer.conversation = route.conversation;
        member.info.state = CallParticipantState::Ringing;
        group->members.push_back(std::move(member));
    }
    if (group->members.empty())
        return false;
    m_group = std::move(group);
    m_peer = m_group->members.front().info.peer;
    m_direction = CallDirection::Outgoing;
    m_callId = CallId::generate();
    m_secret = secret;
    m_codec = m_config.preferredCodec;
    m_endReason = CallEndReason::None;
    m_muted = false;

    (void)openPlayback();
    if (m_soundsEnabled)
        m_sounds.startLoop(CallSound::Ringback);
    m_ringTimer->start(m_config.ringTimeoutMs);
    setState(CallState::Dialing);
    emit participantsChanged();
    // One offer per member, all carrying the same call id and secret: every
    // member keys its pair paths from that one secret and the device ids.
    broadcast(CallSignalMessage::offer(*m_callId, m_secret, m_codec));
    return true;
}

void CallEngine::acceptCall()
{
    if (m_state != CallState::Ringing || m_direction != CallDirection::Incoming || !m_callId)
        return;
    m_ringTimer->stop();
    // The ring stops the instant it is answered, and the pick-up sound marks
    // the line opening.
    m_sounds.stopLoop();
    if (m_soundsEnabled)
        m_sounds.playOnce(CallSound::Connected);
    setState(CallState::Connecting);
    if (m_group) {
        // Media first, then the answer to everyone: the members already in the
        // call start sending the moment they hear it, and those still ringing
        // learn we are in so they key a path to us when they pick up.
        if (!startGroupMedia())
            return;
        broadcast(CallSignalMessage::answer(*m_callId, /*accepted=*/true, m_codec));
        m_stallTimer->start(m_config.connectTimeoutMs);
        return;
    }
    // Media comes up before the answer goes out, so the caller's first frames
    // have a session to land in however quickly the answer reaches them. A
    // failure here ends the call and tells the peer, so no answer is sent.
    if (!startMedia())
        return;
    // Answer with the codec actually agreed, which may be narrower than the one
    // offered if this build lacks it. The caller re-keys on what comes back.
    send(CallSignalMessage::answer(*m_callId, /*accepted=*/true, m_codec));
    m_stallTimer->start(m_config.connectTimeoutMs);
}

void CallEngine::declineCall()
{
    if (m_state != CallState::Ringing || m_direction != CallDirection::Incoming || !m_callId)
        return;
    if (m_group)
        broadcast(CallSignalMessage::answer(*m_callId, /*accepted=*/false, m_codec));
    else
        send(CallSignalMessage::answer(*m_callId, /*accepted=*/false, m_codec));
    endCall(CallEndReason::Declined, /*notifyPeer=*/false);
}

void CallEngine::hangUp()
{
    if (!callOccupiesDevice(m_state))
        return;
    endCall(CallEndReason::LocalHangup, /*notifyPeer=*/true);
}

void CallEngine::setMicrophone(const MicrophoneProcessor::Config &config)
{
    m_microphone.setConfig(config);
}

void CallEngine::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    if (m_session)
        m_session->setMuted(muted);
    if (m_group)
        for (Member &member : m_group->members)
            if (member.session)
                member.session->setMuted(muted);
    // Low for off, high for on: the direction is audible without looking away
    // from whatever prompted the mute.
    if (m_soundsEnabled)
        m_sounds.playOnce(muted ? CallSound::Muted : CallSound::Unmuted);
    emit mutedChanged();
}

void CallEngine::setSoundsEnabled(bool enabled)
{
    if (m_soundsEnabled == enabled)
        return;
    m_soundsEnabled = enabled;
    if (!enabled)
        m_sounds.stopAll();
    else if (m_state == CallState::Dialing || m_state == CallState::Ringing)
        m_sounds.startLoop(m_direction == CallDirection::Outgoing ? CallSound::Ringback
                                                                  : CallSound::IncomingRing);
}

void CallEngine::dismissEndedCall()
{
    if (m_state != CallState::Ended)
        return;
    m_endReason = CallEndReason::None;
    const bool wasGroup = m_group != nullptr;
    m_group.reset();
    setState(CallState::Idle);
    if (wasGroup)
        emit participantsChanged();
}

void CallEngine::updatePeerIdentity(const QString &displayName, const QString &avatarKey)
{
    if (m_state == CallState::Idle)
        return;
    bool changed = false;
    if (!displayName.isEmpty() && displayName != m_peer.displayName) {
        m_peer.displayName = displayName;
        changed = true;
    }
    if (!avatarKey.isEmpty() && avatarKey != m_peer.avatarKey) {
        m_peer.avatarKey = avatarKey;
        changed = true;
    }
    if (changed)
        emit stateChanged();
}

void CallEngine::updateParticipantIdentity(const DeviceId &device, const QString &displayName,
                                           const QString &avatarKey)
{
    Member *member = memberFor(device);
    if (member == nullptr)
        return;
    bool changed = false;
    if (!displayName.isEmpty() && displayName != member->info.peer.displayName) {
        member->info.peer.displayName = displayName;
        changed = true;
    }
    if (!avatarKey.isEmpty() && avatarKey != member->info.peer.avatarKey) {
        member->info.peer.avatarKey = avatarKey;
        changed = true;
    }
    if (member->info.peer.device == m_peer.device)
        updatePeerIdentity(displayName, avatarKey);
    if (changed)
        emit participantsChanged();
}

void CallEngine::send(const CallSignalMessage &message)
{
    send(message, m_peer);
}

void CallEngine::send(const CallSignalMessage &message, const CallPeer &peer)
{
    m_transport.sendSignal(peer.conversation, peer.device, encodeCallSignal(message));
}

void CallEngine::broadcast(const CallSignalMessage &message)
{
    if (!m_group)
        return;
    // Snapshot the recipients: a loopback transport can answer synchronously,
    // and an answer may add or drop members while we are still iterating.
    QVector<CallPeer> recipients;
    for (const Member &member : m_group->members)
        if (participantIsPending(member.info.state))
            recipients.append(member.info.peer);
    for (const CallPeer &peer : recipients)
        send(message, peer);
}

void CallEngine::setParticipantState(Member &member, CallParticipantState state)
{
    if (member.info.state == state)
        return;
    member.info.state = state;
    if (state != CallParticipantState::Joined)
        closeMemberMedia(member);
    emit participantsChanged();
}

void CallEngine::onSignal(const ConversationId &conversation, const DeviceId &sender,
                          const QByteArray &payload)
{
    const std::optional<CallSignalMessage> message = decodeCallSignal(payload);
    if (!message)
        return;

    if (message->type == CallSignalType::Offer) {
        handleOffer(conversation, sender, *message);
        return;
    }

    // Every other signal only makes sense for the call in progress. Matching on
    // the call id (not merely the sender) is what stops a stale hangup from a
    // just-finished call tearing down the one that replaced it.
    if (!m_callId || message->callId != *m_callId || m_state == CallState::Idle)
        return;

    if (m_group) {
        if (conversation != m_group->conversation)
            return;
        Member *member = memberFor(sender);
        if (member == nullptr)
            return;
        switch (message->type) {
        case CallSignalType::Ringing:
            // Their device is alerting; for the caller that is the moment the
            // call goes from dialling into the void to actually ringing.
            if (m_state == CallState::Dialing)
                setState(CallState::Ringing);
            break;
        case CallSignalType::Answer:
            handleGroupAnswer(*member, *message);
            break;
        case CallSignalType::Hangup:
            handleGroupHangup(*member, *message);
            break;
        case CallSignalType::Offer:
            break; // handled above
        }
        return;
    }

    if (sender != m_peer.device)
        return;

    switch (message->type) {
    case CallSignalType::Ringing:
        handleRinging(*message);
        break;
    case CallSignalType::Answer:
        handleAnswer(*message);
        break;
    case CallSignalType::Hangup:
        handleHangup(*message);
        break;
    case CallSignalType::Offer:
        break; // handled above
    }
}

void CallEngine::handleOffer(const ConversationId &conversation, const DeviceId &sender,
                             const CallSignalMessage &message)
{
    // An offer on a group conversation rings the group; the resolver is what
    // tells a group apart from a peer, and who else is in it.
    if (groupRouteResolver) {
        if (const auto route = groupRouteResolver(conversation)) {
            handleGroupOffer(conversation, sender, message, *route);
            return;
        }
    }

    // A redelivered copy of the offer we are already ringing on. The signalling
    // path retries, so this is expected rather than exceptional: re-acknowledge
    // and change nothing.
    if (m_callId && message.callId == *m_callId && m_state == CallState::Ringing
        && m_direction == CallDirection::Incoming) {
        send(CallSignalMessage::ringing(message.callId));
        return;
    }

    CallPeer offeringPeer;
    offeringPeer.conversation = conversation;
    offeringPeer.device = sender;

    if (callOccupiesDevice(m_state)) {
        // Glare: both ends called at the same instant. Some deterministic rule
        // has to pick a winner, and it must pick the SAME winner on both
        // machines from information both already have. Comparing the two call
        // ids does that; the lower id survives.
        const bool isGlare = !m_group && m_direction == CallDirection::Outgoing
            && m_state == CallState::Dialing && sender == m_peer.device && m_callId;
        if (isGlare && message.callId.bytes() < m_callId->bytes()) {
            // We lose: abandon our outgoing call and present theirs instead.
            // The ringback stops here; the incoming ring starts below.
            send(CallSignalMessage::hangup(*m_callId, CallEndReason::Superseded));
            stopCapture();
            m_sounds.stopLoop();
            m_ringTimer->stop();
        } else {
            // Either we won the tie-break or we are simply busy elsewhere. Say
            // so explicitly rather than leaving the peer ringing into a void.
            send(CallSignalMessage::hangup(message.callId, CallEndReason::Busy), offeringPeer);
            return;
        }
    }

    // An offer whose codec this build cannot run is answered on PCM, which every
    // build has, rather than refused.
    m_group.reset();
    m_peer = offeringPeer;
    m_peer.contactId.clear();
    m_direction = CallDirection::Incoming;
    m_callId = message.callId;
    m_secret = message.secret;
    m_codec = isAudioCodecAvailable(message.codec) ? message.codec : AudioCodecKind::Pcm;
    m_endReason = CallEndReason::None;
    m_muted = false;

    (void)openPlayback();
    if (m_soundsEnabled)
        m_sounds.startLoop(CallSound::IncomingRing);
    m_ringTimer->start(m_config.ringTimeoutMs);
    setState(CallState::Ringing);
    send(CallSignalMessage::ringing(message.callId));
    emit incomingCall();
}

void CallEngine::handleGroupOffer(const ConversationId &conversation, const DeviceId &sender,
                                  const CallSignalMessage &message, const GroupCallRoute &route)
{
    CallPeer offeringPeer;
    offeringPeer.conversation = conversation;
    offeringPeer.device = sender;

    // Redelivered offer for the call we are already ringing on: re-acknowledge.
    if (m_group && m_callId && message.callId == *m_callId && m_state == CallState::Ringing
        && m_direction == CallDirection::Incoming) {
        send(CallSignalMessage::ringing(message.callId), offeringPeer);
        return;
    }
    // Without our own device id there is no way to key a pair path; refuse
    // rather than ring a call that could never carry media.
    if (!m_config.localDevice) {
        send(CallSignalMessage::hangup(message.callId, CallEndReason::Busy), offeringPeer);
        return;
    }

    if (callOccupiesDevice(m_state)) {
        // Glare within the group: two members started a call at once. The lower
        // call id survives on every device, so whoever loses abandons theirs.
        const bool isGlare = m_group && m_group->conversation == conversation
            && m_direction == CallDirection::Outgoing
            && (m_state == CallState::Dialing || m_state == CallState::Ringing) && m_callId;
        if (isGlare && message.callId.bytes() < m_callId->bytes()) {
            broadcast(CallSignalMessage::hangup(*m_callId, CallEndReason::Superseded));
            stopCapture();
            m_sounds.stopLoop();
            m_ringTimer->stop();
        } else {
            send(CallSignalMessage::hangup(message.callId, CallEndReason::Busy), offeringPeer);
            return;
        }
    }

    auto group = std::make_unique<GroupCall>();
    group->conversation = conversation;
    group->title = route.title;
    bool callerListed = false;
    for (const CallPeer &peer : route.members) {
        if (peer.device == *m_config.localDevice)
            continue;
        Member member;
        member.info.peer = peer;
        member.info.peer.conversation = conversation;
        // The caller is in the call by definition; everyone else is being rung
        // alongside us and will say what they did.
        if (peer.device == sender) {
            member.info.state = CallParticipantState::Joined;
            member.codec = message.codec;
            callerListed = true;
        } else {
            member.info.state = CallParticipantState::Ringing;
        }
        group->members.push_back(std::move(member));
    }
    if (!callerListed) {
        // A member we have not learned about yet (their roster update is still
        // on its way). Ring anyway: the MLS group already vouched for them.
        Member member;
        member.info.peer = offeringPeer;
        member.info.state = CallParticipantState::Joined;
        member.codec = message.codec;
        group->members.insert(group->members.begin(), std::move(member));
    }
    m_group = std::move(group);
    m_peer = offeringPeer;
    for (const Member &member : m_group->members)
        if (member.info.peer.device == sender)
            m_peer = member.info.peer;
    m_direction = CallDirection::Incoming;
    m_callId = message.callId;
    m_secret = message.secret;
    m_codec = isAudioCodecAvailable(message.codec) ? message.codec : AudioCodecKind::Pcm;
    m_endReason = CallEndReason::None;
    m_muted = false;

    (void)openPlayback();
    if (m_soundsEnabled)
        m_sounds.startLoop(CallSound::IncomingRing);
    m_ringTimer->start(m_config.ringTimeoutMs);
    setState(CallState::Ringing);
    emit participantsChanged();
    send(CallSignalMessage::ringing(message.callId), m_peer);
    emit incomingCall();
}

void CallEngine::handleRinging(const CallSignalMessage &)
{
    // Only meaningful while we are still dialling; a late one changes nothing.
    if (m_state == CallState::Dialing)
        setState(CallState::Ringing);
}

void CallEngine::handleAnswer(const CallSignalMessage &message)
{
    if (m_direction != CallDirection::Outgoing)
        return;
    if (m_state != CallState::Dialing && m_state != CallState::Ringing)
        return;
    m_ringTimer->stop();
    if (!message.accepted) {
        endCall(CallEndReason::Declined, /*notifyPeer=*/false);
        return;
    }
    // Adopt the codec the callee agreed to, so both ends key and decode the same
    // way even when one of them lacks the offered codec.
    m_codec = isAudioCodecAvailable(message.codec) ? message.codec : AudioCodecKind::Pcm;
    // They picked up: the ringback stops and the same pick-up sound the callee
    // heard plays here too, so both ends hear the line open.
    m_sounds.stopLoop();
    if (m_soundsEnabled)
        m_sounds.playOnce(CallSound::Connected);
    setState(CallState::Connecting);
    if (!startMedia())
        return;
    m_stallTimer->start(m_config.connectTimeoutMs);
}

void CallEngine::handleGroupAnswer(Member &member, const CallSignalMessage &message)
{
    if (!message.accepted) {
        setParticipantState(member, CallParticipantState::Declined);
        settleGroup();
        return;
    }
    // An answer we already acted on (the join handshake is deliberately
    // idempotent: members re-announce themselves to newcomers).
    if (member.info.state == CallParticipantState::Joined && member.session)
        return;
    member.codec = message.codec;
    const bool wasJoined = member.info.state == CallParticipantState::Joined;
    member.info.state = CallParticipantState::Joined;

    const bool weAreIn = m_state == CallState::Connecting || m_state == CallState::Active;
    const bool weAreCalling = m_direction == CallDirection::Outgoing
        && (m_state == CallState::Dialing || m_state == CallState::Ringing);
    if (weAreCalling) {
        // The first pick-up opens the line; later ones just join it. The ring
        // keeps running for whoever has not answered yet.
        m_sounds.stopLoop();
        if (m_soundsEnabled)
            m_sounds.playOnce(CallSound::Connected);
        setState(CallState::Connecting);
        if (!startGroupMedia())
            return;
        m_stallTimer->start(m_config.connectTimeoutMs);
    } else if (weAreIn) {
        if (!openMemberMedia(member)) {
            endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
            return;
        }
        // Tell the newcomer we are here too, unicast. They mark us Joined and
        // key their side of the pair; if they already knew, they ignore it.
        send(CallSignalMessage::answer(*m_callId, /*accepted=*/true, m_codec), member.info.peer);
    }
    // Otherwise we are still ringing ourselves: remember that they are in, so
    // accepting keys a path to them as well as to the caller.
    if (!wasJoined)
        emit participantsChanged();
}

void CallEngine::handleHangup(const CallSignalMessage &message)
{
    const CallEndReason reason = [&] {
        switch (message.reason) {
        case CallEndReason::Busy:
            return CallEndReason::Busy;
        case CallEndReason::Declined:
            return CallEndReason::Declined;
        case CallEndReason::Superseded:
            return CallEndReason::Superseded;
        default:
            // Anything else is the peer ending the call, whatever it called it.
            return CallEndReason::RemoteHangup;
        }
    }();
    endCall(reason, /*notifyPeer=*/false);
}

void CallEngine::handleGroupHangup(Member &member, const CallSignalMessage &message)
{
    const CallParticipantState state = [&] {
        switch (message.reason) {
        case CallEndReason::Busy:
            return CallParticipantState::Busy;
        case CallEndReason::Declined:
            return CallParticipantState::Declined;
        case CallEndReason::NoAnswer:
        case CallEndReason::Unanswered:
            return CallParticipantState::NoAnswer;
        default:
            return member.info.state == CallParticipantState::Ringing
                ? CallParticipantState::NoAnswer
                : CallParticipantState::Left;
        }
    }();
    setParticipantState(member, state);
    settleGroup();
}

void CallEngine::settleGroup()
{
    if (!m_group || !callOccupiesDevice(m_state))
        return;
    int joined = 0;
    int ringing = 0;
    bool anyDeclined = false;
    bool anyBusy = false;
    for (const Member &member : m_group->members) {
        switch (member.info.state) {
        case CallParticipantState::Joined:
            ++joined;
            break;
        case CallParticipantState::Ringing:
            ++ringing;
            break;
        case CallParticipantState::Declined:
            anyDeclined = true;
            break;
        case CallParticipantState::Busy:
            anyBusy = true;
            break;
        case CallParticipantState::Left:
        case CallParticipantState::NoAnswer:
            break;
        }
    }
    if (m_direction == CallDirection::Incoming && m_state == CallState::Ringing) {
        // Still deciding, and everyone who was in the call has gone: nothing
        // left to answer. The offer went unanswered from our point of view.
        if (joined == 0)
            endCall(CallEndReason::RemoteHangup, /*notifyPeer=*/false);
        return;
    }
    if (joined > 0 || ringing > 0)
        return;
    if (m_direction == CallDirection::Outgoing
        && (m_state == CallState::Dialing || m_state == CallState::Ringing)) {
        endCall(anyDeclined ? CallEndReason::Declined
                            : anyBusy ? CallEndReason::Busy : CallEndReason::NoAnswer,
                /*notifyPeer=*/false);
        return;
    }
    // We were in the call and the last other member left it.
    endCall(CallEndReason::RemoteHangup, /*notifyPeer=*/false);
}

void CallEngine::onRingTimeout()
{
    if (m_group && m_direction == CallDirection::Outgoing) {
        // Whoever has not picked up by now is not going to. The call itself
        // only ends if nobody did.
        for (Member &member : m_group->members)
            if (member.info.state == CallParticipantState::Ringing)
                setParticipantState(member, CallParticipantState::NoAnswer);
        settleGroup();
        return;
    }
    // Whose "no answer" this is depends on the direction: the caller gave up
    // waiting; the callee never picked up and records a missed call.
    endCall(m_direction == CallDirection::Outgoing ? CallEndReason::NoAnswer
                                                   : CallEndReason::Unanswered,
            /*notifyPeer=*/true);
}

void CallEngine::onMedia(const ConversationId &conversation, const DeviceId &sender,
                         const QByteArray &packet)
{
    if (m_state != CallState::Connecting && m_state != CallState::Active)
        return;
    if (m_group) {
        if (conversation != m_group->conversation)
            return;
        if (Member *member = memberFor(sender); member != nullptr && member->session)
            onGroupMedia(*member, packet);
        return;
    }
    if (!m_session || sender != m_peer.device || conversation != m_peer.conversation)
        return;

    if (!packet.isEmpty() && static_cast<quint8>(packet[0]) == CallScreenSession::wireVersion) {
        if (!m_screenSession)
            return;
        qint64 lastSeenMs = 0;
        const ScreenPacketOutcome outcome =
            handleScreenPacket(*m_screenSession, packet, m_remoteScreenActive, lastSeenMs);
        if (m_remoteScreenActive)
            m_screenTimeout->start(screenStaleMs);
        else
            m_screenTimeout->stop();
        if (outcome.ended || outcome.changed)
            emit remoteScreenFrame(outcome.canvas);
        return;
    }
    if (!packet.isEmpty() && static_cast<quint8>(packet[0]) == CallVideoSession::wireVersion) {
        if (m_videoSession) {
            const auto image = m_videoSession->decode(packet);
            if (image) {
                if (image->isNull())
                    m_videoTimeout->stop();
                else
                    m_videoTimeout->start(videoStaleMs);
                emit remoteVideoFrame(*image);
            }
        }
        return;
    }
    const CallSession::ReceiveResult result = m_session->processIncomingPacket(packet);
    if (result != CallSession::ReceiveResult::Queued)
        return;

    // The first authenticated packet is what proves the media path works in this
    // direction, so it — not the answer — is what promotes the call to Active.
    m_lastMediaMs = QDateTime::currentMSecsSinceEpoch();
    if (m_state == CallState::Connecting) {
        m_activeSinceMs = m_lastMediaMs;
        setState(CallState::Active);
    }
    m_stallTimer->start(m_config.mediaStallTimeoutMs);
}

void CallEngine::onGroupMedia(Member &member, const QByteArray &packet)
{
    if (!packet.isEmpty() && static_cast<quint8>(packet[0]) == CallScreenSession::wireVersion) {
        if (!member.screen)
            return;
        const ScreenPacketOutcome outcome =
            handleScreenPacket(*member.screen, packet, member.screenOn, member.lastScreenMs);
        if (outcome.ended || outcome.changed)
            emit participantScreenFrame(member.info.peer.device, outcome.canvas);
        return;
    }
    if (!packet.isEmpty() && static_cast<quint8>(packet[0]) == CallVideoSession::wireVersion) {
        if (!member.video)
            return;
        const auto image = member.video->decode(packet);
        if (!image)
            return;
        member.cameraOn = !image->isNull();
        member.lastVideoMs = QDateTime::currentMSecsSinceEpoch();
        emit participantVideoFrame(member.info.peer.device, *image);
        return;
    }
    if (member.session->processIncomingPacket(packet) != CallSession::ReceiveResult::Queued)
        return;
    m_lastMediaMs = QDateTime::currentMSecsSinceEpoch();
    if (m_state == CallState::Connecting) {
        m_activeSinceMs = m_lastMediaMs;
        setState(CallState::Active);
    }
    m_stallTimer->start(m_config.mediaStallTimeoutMs);
}

bool CallEngine::openPlayback()
{
    m_playbackTailTimer->stop(); // a new call reclaims the speaker
    if (m_playback)
        return true;
    if (!m_audioIo.isValid())
        return false;
    m_playback = m_audioIo.makePlayback();
    if (!m_playback)
        return false;
    m_playback->pullFrame = [this] { return pullPlaybackFrame(); };
    if (!m_playback->start()) {
        m_playback->pullFrame = nullptr;
        m_playback.reset();
        return false;
    }
    return true;
}

void CallEngine::closePlayback()
{
    if (!m_playback)
        return;
    m_playback->pullFrame = nullptr;
    m_playback->stop();
    m_playback.reset();
}

bool CallEngine::startCapture()
{
    // The speaker is normally already up from the ring; retry here so a call
    // that could not open it earlier still gets a chance to be heard.
    if (!openPlayback())
        return false;
    if (m_capture)
        return true;
    m_capture = m_audioIo.makeCapture();
    if (!m_capture)
        return false;
    // A fresh device means a fresh gate: nothing from the last call's tail
    // should decide whether this call's first words get through.
    m_microphone.reset();
    m_capture->onFrame = [this](const AudioFrame &frame) { onCapturedFrame(frame); };
    if (!m_capture->start()) {
        m_capture->onFrame = nullptr;
        m_capture.reset();
        return false;
    }
    m_levelTimer->start();
    return true;
}

bool CallEngine::startMedia()
{
    if (!m_callId || !m_audioIo.isValid()) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }

    CallSession::Config sessionConfig;
    sessionConfig.callId = *m_callId;
    sessionConfig.direction = m_direction;
    sessionConfig.codec = m_codec;
    sessionConfig.jitter.clock = m_config.mediaClock;
    m_session = CallSession::create(sessionConfig, m_secret);
    if (!m_session) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    m_videoSession = CallVideoSession::create(*m_callId, m_direction, m_secret);
    // The encoder allocates nothing until a desktop is actually handed to it,
    // so a call that never shares a screen pays for an empty object and no more.
    m_screenEncoder = std::make_shared<ScreenTileEncoder>();
    m_screenSession =
        CallScreenSession::create(*m_callId, m_direction, m_secret, m_screenEncoder);
    m_screenFeedbackTimer->start();
    m_session->setMuted(m_muted);

    if (!startCapture()) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    return true;
}

bool CallEngine::openMemberMedia(Member &member)
{
    if (member.session)
        return true;
    if (!m_callId || !m_config.localDevice)
        return false;
    // Each pair keys its own path from the call secret and both device ids,
    // and takes its direction from their order, so the two ends of a pair
    // always land on opposite halves of the same schedule.
    const QByteArray pairSecret = deriveGroupPairSecret(m_secret, *m_callId, *m_config.localDevice,
                                                        member.info.peer.device);
    if (pairSecret.isEmpty())
        return false;
    const CallDirection direction = m_config.localDevice->bytes() < member.info.peer.device.bytes()
        ? CallDirection::Outgoing
        : CallDirection::Incoming;
    CallSession::Config sessionConfig;
    sessionConfig.callId = *m_callId;
    sessionConfig.direction = direction;
    sessionConfig.codec = pairCodec(member.codec);
    sessionConfig.jitter.clock = m_config.mediaClock;
    member.session = CallSession::create(sessionConfig, pairSecret);
    if (!member.session)
        return false;
    member.session->setMuted(m_muted);
    member.video = CallVideoSession::create(*m_callId, direction, pairSecret);
    if (!m_screenEncoder)
        m_screenEncoder = std::make_shared<ScreenTileEncoder>();
    member.screen =
        CallScreenSession::create(*m_callId, direction, pairSecret, m_screenEncoder);
    m_screenFeedbackTimer->start();
    return true;
}

void CallEngine::closeMemberMedia(Member &member)
{
    if (member.cameraOn) {
        member.cameraOn = false;
        emit participantVideoFrame(member.info.peer.device, QImage());
    }
    if (member.screenOn) {
        member.screenOn = false;
        emit participantScreenFrame(member.info.peer.device, {});
    }
    member.screen.reset();
    member.video.reset();
    member.session.reset();
    member.info.speaking = false;
    member.info.level = 0.0;
}

bool CallEngine::startGroupMedia()
{
    if (!m_callId || !m_audioIo.isValid() || !m_group) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    for (Member &member : m_group->members) {
        if (member.info.state != CallParticipantState::Joined)
            continue;
        if (!openMemberMedia(member)) {
            endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
            return false;
        }
    }
    if (!startCapture()) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    return true;
}

void CallEngine::stopCapture()
{
    m_levelTimer->stop();
    m_stallTimer->stop();
    // The microphone goes down before the session it feeds: a frame delivered
    // from a half-torn-down device would otherwise reach a destroyed session.
    if (m_capture) {
        m_capture->onFrame = nullptr;
        m_capture->stop();
        m_capture.reset();
    }
    m_videoTimeout->stop();
    m_videoSession.reset();
    emit remoteVideoFrame(QImage());
    // The screen path goes down whole: the capture is the app's, but every
    // buffer, key and canvas the engine holds for it is released here, so a
    // finished call leaves nothing of a share behind.
    m_screenFeedbackTimer->stop();
    m_screenTimeout->stop();
    m_screenSharing = false;
    m_screenSession.reset();
    m_screenEncoder.reset();
    if (m_remoteScreenActive) {
        m_remoteScreenActive = false;
        emit remoteScreenFrame({});
    }
    m_session.reset();
    if (m_group) {
        bool changed = false;
        for (Member &member : m_group->members) {
            if (member.session || member.video || member.info.speaking)
                changed = true;
            closeMemberMedia(member);
        }
        if (changed)
            emit participantsChanged();
    }
    m_secret.fill('\0');
    m_secret.clear();
}

void CallEngine::stopMedia()
{
    stopCapture();
    closePlayback();
}

void CallEngine::onCapturedFrame(const AudioFrame &captured)
{
    if (m_state != CallState::Connecting && m_state != CallState::Active)
        return;
    // Gain and gate once, here, so every session below encodes the same
    // frame and a closed gate sends real silence to the whole mesh.
    const AudioFrame frame = m_microphone.process(captured);
    if (m_group) {
        // The same frame, sealed separately for each member: every pair has its
        // own key, and nobody's audio is ever forwarded by a third device.
        for (Member &member : m_group->members) {
            if (!member.session)
                continue;
            const QByteArray packet = member.session->processCapturedFrame(frame);
            if (packet.isEmpty() || !m_transport.isConnected())
                continue;
            m_transport.sendMedia(m_group->conversation, member.info.peer.device, packet);
        }
        return;
    }
    if (!m_session)
        return;
    const QByteArray packet = m_session->processCapturedFrame(frame);
    if (packet.isEmpty() || !m_transport.isConnected())
        return;
    m_transport.sendMedia(m_peer.conversation, m_peer.device, packet);
}

void CallEngine::sendVideoFrame(const QImage &image)
{
    if (!m_transport.isConnected()
        || (m_state != CallState::Connecting && m_state != CallState::Active))
        return;
    if (m_group) {
        for (Member &member : m_group->members) {
            if (!member.video)
                continue;
            const QByteArray packet = member.video->encode(image);
            if (!packet.isEmpty())
                m_transport.sendMedia(m_group->conversation, member.info.peer.device, packet);
        }
        return;
    }
    if (!m_videoSession)
        return;
    const QByteArray packet = m_videoSession->encode(image);
    if (!packet.isEmpty())
        m_transport.sendMedia(m_peer.conversation, m_peer.device, packet);
}

CallEngine::ScreenPacketOutcome CallEngine::handleScreenPacket(CallScreenSession &session,
                                                               const QByteArray &packet,
                                                               bool &activeFlag,
                                                               qint64 &lastSeenMs)
{
    ScreenPacketOutcome outcome;
    // The media clock, not the wall clock: the round-trip and report timing a
    // share adapts on must not jump when the system time is corrected.
    const qint64 now = m_config.mediaClock ? m_config.mediaClock() : steadyClockMs();
    const auto update = session.decode(packet, now);
    if (!update)
        return outcome;
    switch (update->kind) {
    case CallScreenSession::Update::Kind::Feedback:
        // The peer said what it can take and how big its window is. One encoder
        // serves everyone, so it runs at the worst answer anybody gave.
        applyScreenEncoderPolicy();
        break;
    case CallScreenSession::Update::Kind::Stopped:
        if (activeFlag) {
            activeFlag = false;
            outcome.ended = true;
        }
        break;
    case CallScreenSession::Update::Kind::Frame:
        // Staleness is judged on the same wall clock the camera uses, because
        // that is the clock the sweep that checks it runs on.
        lastSeenMs = QDateTime::currentMSecsSinceEpoch();
        // A heartbeat from a motionless desktop carries no tiles and no dirty
        // region; it keeps the share alive without costing the UI a repaint.
        if (!activeFlag || update->canvasReplaced || !update->dirty.isNull()) {
            outcome.changed = true;
            outcome.canvas = update->canvas;
        }
        activeFlag = true;
        break;
    }
    return outcome;
}

bool CallEngine::startScreenShare()
{
    if (!m_screenEncoder || (m_state != CallState::Connecting && m_state != CallState::Active))
        return false;
    m_screenSharing = true;
    return true;
}

void CallEngine::sendScreenFrame(const ScreenFrameView &frame)
{
    if (!m_screenSharing || !m_screenEncoder
        || (m_state != CallState::Connecting && m_state != CallState::Active))
        return;
    // Checked before the frame is looked at, not after: building an update
    // consumes the change set, so doing it with nowhere to send would silently
    // lose whatever moved.
    if (!m_transport.isConnected())
        return;

    const qint64 now = m_config.mediaClock ? m_config.mediaClock() : steadyClockMs();
    const QByteArrayView payload = m_screenEncoder->buildUpdate(frame, now);
    if (payload.isEmpty())
        return;

    if (m_group) {
        // One desktop, one encode, one seal per member: the picture is built
        // once however many people are in the call.
        for (Member &member : m_group->members) {
            if (!member.screen)
                continue;
            const QByteArray packet = member.screen->sealUpdate(payload, now);
            if (!packet.isEmpty())
                m_transport.sendMedia(m_group->conversation, member.info.peer.device, packet);
        }
        return;
    }
    if (!m_screenSession)
        return;
    const QByteArray packet = m_screenSession->sealUpdate(payload, now);
    if (!packet.isEmpty())
        m_transport.sendMedia(m_peer.conversation, m_peer.device, packet);
}

void CallEngine::stopScreenShare()
{
    if (!m_screenSharing)
        return;
    m_screenSharing = false;
    if (m_transport.isConnected()) {
        if (m_group) {
            for (Member &member : m_group->members) {
                if (!member.screen)
                    continue;
                const QByteArray packet = member.screen->encodeStop();
                if (!packet.isEmpty())
                    m_transport.sendMedia(m_group->conversation, member.info.peer.device, packet);
            }
        } else if (m_screenSession) {
            const QByteArray packet = m_screenSession->encodeStop();
            if (!packet.isEmpty())
                m_transport.sendMedia(m_peer.conversation, m_peer.device, packet);
        }
    }
    releaseScreenSender();
}

void CallEngine::releaseScreenSender()
{
    // The call may well continue, so the sessions and their keys stay; what goes
    // is everything the size of a desktop.
    if (m_screenEncoder)
        m_screenEncoder->reset();
    if (m_screenSession)
        m_screenSession->resetSendState();
    if (m_group) {
        for (Member &member : m_group->members) {
            if (member.screen)
                member.screen->resetSendState();
        }
    }
}

int CallEngine::screenShareTargetFps() const
{
    return m_screenEncoder ? m_screenEncoder->targetFps()
                           : screenShareLevels().front().targetFps;
}

void CallEngine::setScreenViewSize(QSize size)
{
    if (m_screenSession)
        m_screenSession->setViewSize(size);
}

void CallEngine::setScreenViewSize(const DeviceId &device, QSize size)
{
    if (Member *member = memberFor(device); member != nullptr && member->screen)
        member->screen->setViewSize(size);
}

ScreenShareStats CallEngine::screenShareStats() const
{
    if (m_screenSession)
        return m_screenSession->stats();
    if (m_group) {
        for (const Member &member : m_group->members) {
            if (member.screen)
                return member.screen->stats();
        }
    }
    return {};
}

void CallEngine::pumpScreenFeedback()
{
    if (!m_transport.isConnected())
        return;
    const qint64 now = m_config.mediaClock ? m_config.mediaClock() : steadyClockMs();
    if (m_screenSession && m_screenSession->isReceiving()) {
        const QByteArray report = m_screenSession->encodeFeedback(now);
        if (!report.isEmpty())
            m_transport.sendMedia(m_peer.conversation, m_peer.device, report);
    }
    if (!m_group)
        return;
    for (Member &member : m_group->members) {
        if (!member.screen || !member.screen->isReceiving())
            continue;
        const QByteArray report = member.screen->encodeFeedback(now);
        if (!report.isEmpty())
            m_transport.sendMedia(m_group->conversation, member.info.peer.device, report);
    }
}

void CallEngine::applyScreenEncoderPolicy()
{
    if (!m_screenEncoder)
        return;
    int worstLevel = 0;
    QSize largestView;
    bool anyoneWatching = false;
    bool sawPeer = false;
    const auto fold = [&](const CallScreenSession &session) {
        sawPeer = true;
        // The worst rung anyone asked for wins: one payload serves every peer,
        // so it has to be one the weakest of them can actually receive.
        worstLevel = std::max(worstLevel, session.desiredLevel());
        if (session.remoteViewHidden())
            return;
        anyoneWatching = true;
        const QSize view = session.remoteViewSize();
        largestView = QSize(std::max(largestView.width(), view.width()),
                            std::max(largestView.height(), view.height()));
    };
    if (m_screenSession)
        fold(*m_screenSession);
    if (m_group) {
        for (const Member &member : m_group->members) {
            if (member.screen)
                fold(*member.screen);
        }
    }
    if (!sawPeer)
        return;
    m_screenEncoder->setLevel(worstLevel);
    m_screenEncoder->setRemoteView(largestView, anyoneWatching);
}

AudioFrame CallEngine::pullPlaybackFrame()
{
    // Before the call is answered there is no session at all and the frame is
    // pure sound; during a call the sounds ride over the far end's voice on the
    // same stream, which is what keeps them on one clock and one device.
    AudioFrame frame;
    if (m_session) {
        frame = m_session->nextPlaybackFrame();
    } else if (m_group) {
        // Everyone in the call, mixed: each member's own jitter buffer paces
        // their voice, and the sum is what one speaker plays.
        frame = silentAudioFrame();
        for (Member &member : m_group->members)
            if (member.session)
                mixInto(frame, member.session->nextPlaybackFrame());
    } else {
        frame = silentAudioFrame();
    }
    m_sounds.mixInto(frame);
    return frame;
}

void CallEngine::refreshParticipantLevels()
{
    if (!m_group)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for (Member &member : m_group->members) {
        const bool speaking = m_state == CallState::Active && member.session
            && member.session->isRemoteSpeaking();
        const double level = member.session ? member.session->remoteLevel() : 0.0;
        if (speaking != member.info.speaking || !qFuzzyCompare(level + 1.0, member.info.level + 1.0))
            changed = true;
        member.info.speaking = speaking;
        member.info.level = level;
        // A camera whose frames stopped arriving reads as off, even if the
        // camera-off packet itself was lost.
        if (member.cameraOn && now - member.lastVideoMs > videoStaleMs) {
            member.cameraOn = false;
            emit participantVideoFrame(member.info.peer.device, QImage());
        }
        // A share heartbeats every second even when nothing moves, so silence
        // this long is a share that ended without its stop packet arriving.
        if (member.screenOn && now - member.lastScreenMs > screenStaleMs) {
            member.screenOn = false;
            if (member.screen)
                member.screen->resetReceiver();
            emit participantScreenFrame(member.info.peer.device, {});
        }
    }
    if (changed)
        emit participantsChanged();
}

void CallEngine::setState(CallState state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void CallEngine::endCall(CallEndReason reason, bool notifyPeer)
{
    if (m_state == CallState::Idle || m_state == CallState::Ended)
        return;
    if (notifyPeer && m_callId) {
        if (m_group)
            broadcast(CallSignalMessage::hangup(*m_callId, reason));
        else
            send(CallSignalMessage::hangup(*m_callId, reason));
    }
    m_ringTimer->stop();
    // The microphone and the call keys go immediately; the speaker stays a
    // moment longer so the hang-up sound is actually heard rather than cut off
    // by its own device closing.
    stopCapture();
    m_sounds.stopLoop();
    if (m_soundsEnabled && m_playback)
        m_sounds.playOnce(CallSound::Ended);
    if (m_playback) {
        m_playbackTailTimer->start(m_sounds.remainingOneShotMs() + playbackTailSlackMs);
    } else {
        m_sounds.stopAll();
    }
    m_callId.reset();
    m_activeSinceMs = 0;
    m_lastMediaMs = 0;
    m_endReason = reason;
    setState(CallState::Ended);
    emit callEnded(reason);
    emit levelsChanged();
}

} // namespace OpenChat
