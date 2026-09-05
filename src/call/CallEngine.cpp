#include "call/CallEngine.h"

#include <QDateTime>
#include <QTimer>

#include <chrono>

namespace OpenChat {

namespace {

// How often the UI's level indicators are refreshed. 20 Hz is smooth to the eye
// and two orders of magnitude cheaper than emitting once per 20 ms audio frame.
constexpr int levelRefreshMs = 50;

// Slack added to the hang-up sound's own length before the speaker is closed.
// Enough to cover the device's own buffering; short enough that nothing is
// holding the audio device open in the background afterwards.
constexpr int playbackTailSlackMs = 250;

} // namespace

qint64 steadyClockMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

CallEngine::CallEngine(Config config, CallTransport &transport, CallAudioIoFactory audioIo,
                       QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_transport(transport)
    , m_audioIo(std::move(audioIo))
{
    if (!isAudioCodecAvailable(m_config.preferredCodec))
        m_config.preferredCodec = AudioCodecKind::Pcm;

    m_videoTimeout = new QTimer(this);
    m_videoTimeout->setSingleShot(true);
    connect(m_videoTimeout, &QTimer::timeout, this, [this] { emit remoteVideoFrame(QImage()); });

    m_ringTimer = new QTimer(this);
    m_ringTimer->setSingleShot(true);
    connect(m_ringTimer, &QTimer::timeout, this, [this] {
        // Whose "no answer" this is depends on the direction: the caller gave up
        // waiting; the callee never picked up and records a missed call.
        endCall(m_direction == CallDirection::Outgoing ? CallEndReason::NoAnswer
                                                       : CallEndReason::Unanswered,
                /*notifyPeer=*/true);
    });

    m_stallTimer = new QTimer(this);
    m_stallTimer->setSingleShot(true);
    connect(m_stallTimer, &QTimer::timeout, this,
            [this] { endCall(CallEndReason::TransportFailed, /*notifyPeer=*/true); });

    m_levelTimer = new QTimer(this);
    m_levelTimer->setInterval(levelRefreshMs);
    connect(m_levelTimer, &QTimer::timeout, this, &CallEngine::levelsChanged);

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
    return m_session ? m_session->localLevel() : 0.0;
}

double CallEngine::remoteLevel() const
{
    return m_session ? m_session->remoteLevel() : 0.0;
}

bool CallEngine::isLocalSpeaking() const
{
    return m_state == CallState::Active && m_session && m_session->isLocalSpeaking();
}

bool CallEngine::isRemoteSpeaking() const
{
    return m_state == CallState::Active && m_session && m_session->isRemoteSpeaking();
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
    send(CallSignalMessage::answer(*m_callId, /*accepted=*/false, m_codec));
    endCall(CallEndReason::Declined, /*notifyPeer=*/false);
}

void CallEngine::hangUp()
{
    if (!callOccupiesDevice(m_state))
        return;
    endCall(CallEndReason::LocalHangup, /*notifyPeer=*/true);
}

void CallEngine::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    if (m_session)
        m_session->setMuted(muted);
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
    setState(CallState::Idle);
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

void CallEngine::send(const CallSignalMessage &message)
{
    send(message, m_peer);
}

void CallEngine::send(const CallSignalMessage &message, const CallPeer &peer)
{
    m_transport.sendSignal(peer.conversation, peer.device, encodeCallSignal(message));
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
        const bool isGlare = m_direction == CallDirection::Outgoing
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

void CallEngine::onMedia(const ConversationId &conversation, const DeviceId &sender,
                         const QByteArray &packet)
{
    if (!m_session || sender != m_peer.device || conversation != m_peer.conversation)
        return;
    if (m_state != CallState::Connecting && m_state != CallState::Active)
        return;

    if (!packet.isEmpty() && static_cast<quint8>(packet[0]) == CallVideoSession::wireVersion) {
        if (m_videoSession) {
            const auto image = m_videoSession->decode(packet);
            if (image) {
                if (image->isNull())
                    m_videoTimeout->stop();
                else
                    m_videoTimeout->start(2500);
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
    m_session->setMuted(m_muted);

    // The speaker is normally already up from the ring; retry here so a call
    // that could not open it earlier still gets a chance to be heard.
    if (!openPlayback()) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    m_capture = m_audioIo.makeCapture();
    if (!m_capture) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    m_capture->onFrame = [this](const AudioFrame &frame) { onCapturedFrame(frame); };
    if (!m_capture->start()) {
        endCall(CallEndReason::SetupFailed, /*notifyPeer=*/true);
        return false;
    }
    m_levelTimer->start();
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
    m_session.reset();
    m_secret.fill('\0');
    m_secret.clear();
}

void CallEngine::stopMedia()
{
    stopCapture();
    closePlayback();
}

void CallEngine::onCapturedFrame(const AudioFrame &frame)
{
    if (!m_session || (m_state != CallState::Connecting && m_state != CallState::Active))
        return;
    const QByteArray packet = m_session->processCapturedFrame(frame);
    if (packet.isEmpty() || !m_transport.isConnected())
        return;
    m_transport.sendMedia(m_peer.conversation, m_peer.device, packet);
}

void CallEngine::sendVideoFrame(const QImage &image)
{
    if (!m_videoSession || !m_transport.isConnected()
        || (m_state != CallState::Connecting && m_state != CallState::Active))
        return;
    const QByteArray packet = m_videoSession->encode(image);
    if (!packet.isEmpty())
        m_transport.sendMedia(m_peer.conversation, m_peer.device, packet);
}

AudioFrame CallEngine::pullPlaybackFrame()
{
    // Before the call is answered there is no session at all and the frame is
    // pure sound; during a call the sounds ride over the far end's voice on the
    // same stream, which is what keeps them on one clock and one device.
    AudioFrame frame = m_session ? m_session->nextPlaybackFrame() : silentAudioFrame();
    m_sounds.mixInto(frame);
    return frame;
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
    if (notifyPeer && m_callId)
        send(CallSignalMessage::hangup(*m_callId, reason));
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
