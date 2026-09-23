#include "controllers/VoiceDebugController.h"

#include "app/MicrophoneSettings.h"
#include "app/TransportSettings.h"
#include "call/CallEngine.h"
#include "call/UdpCallMediaPath.h"
#include "controllers/CallController.h"

#include <QDateTime>
#include <QTime>
#include <cmath>

namespace OpenChat {

namespace {

QString formatDuration(qint64 ms)
{
    const qint64 totalSeconds = ms / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString peerStateName(UdpPeerState state)
{
    switch (state) {
    case UdpPeerState::Probing:
        return QStringLiteral("Probing");
    case UdpPeerState::Active:
        return QStringLiteral("Active");
    case UdpPeerState::Suspended:
        return QStringLiteral("Suspended");
    }
    return QStringLiteral("Unknown");
}

} // namespace

VoiceDebugController::VoiceDebugController(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &VoiceDebugController::onRefreshTick);
    m_timer->start();

    logEvent(QStringLiteral("SYSTEM"),
             QStringLiteral("Voice Debug Diagnostics Controller initialized (100ms refresh)"),
             QStringLiteral("INFO"));
}

VoiceDebugController::~VoiceDebugController() = default;

void VoiceDebugController::setLiveSources(CallEngine *engine, UdpCallMediaPath *udpPath,
                                          TransportSettings *transportSettings,
                                          MicrophoneSettings *micSettings)
{
    m_callEngine = engine;
    m_udpPath = udpPath;
    m_transportSettings = transportSettings;
    m_micSettings = micSettings;
    m_previewMode = false;

    if (m_udpPath) {
        connect(m_udpPath, &UdpCallMediaPath::diagnosticEventLogged, this,
                &VoiceDebugController::onUdpDiagnosticEvent);
        connect(m_udpPath, &UdpCallMediaPath::lagSpikeDetected, this,
                &VoiceDebugController::onUdpLagSpike);
        logEvent(QStringLiteral("CARRIER"),
                 QStringLiteral("Connected to live UdpCallMediaPath (relay %1:%2)")
                     .arg(m_udpPath->relayAddress().toString())
                     .arg(m_udpPath->relayPort()),
                 QStringLiteral("INFO"));
    }

    if (m_callEngine) {
        logEvent(QStringLiteral("ENGINE"),
                 QStringLiteral("Connected to live CallEngine (Codec: %1)")
                     .arg(m_callEngine->currentCodec() == AudioCodecKind::Opus ? "Opus 48kHz"
                                                                              : "PCM 48kHz"),
                 QStringLiteral("INFO"));
    }
}

void VoiceDebugController::enableForPreview(CallController *previewCallCtrl)
{
    m_previewMode = true;
    m_previewCallCtrl = previewCallCtrl;
    m_hasCall = true;
    m_callState = QStringLiteral("Active");
    m_callDirection = QStringLiteral("Incoming");
    m_callId = QStringLiteral("PREVIEW-CALL-7A2F-9C14");
    m_codecName = QStringLiteral("Opus 48kHz (Mono, 20ms frames)");
    m_carrierMode = QStringLiteral("UDP (Relay-assisted)");
    m_peerUdpState = QStringLiteral("Active");
    m_relayEndpoint = QStringLiteral("127.0.0.1:8444");
    m_localPort = 53820;
    m_transportSetting = QStringLiteral("Auto");
    m_targetDepth = 4;
    m_peakDepth = 6;
    m_jitterMs = 6;

    logEvent(QStringLiteral("PREVIEW"),
             QStringLiteral("Voice Debug Overlay initialized in PREVIEW mode"),
             QStringLiteral("INFO"));
    logEvent(QStringLiteral("CARRIER"),
             QStringLiteral("Mock Relay UDP media path bound at 127.0.0.1:8444 (local :53820)"),
             QStringLiteral("INFO"));
    logEvent(QStringLiteral("CARRIER"),
             QStringLiteral("Peer state: Active (direct UDP relay established)"),
             QStringLiteral("INFO"));
    logEvent(QStringLiteral("CODEC"),
             QStringLiteral("Opus 48kHz mono active, 20ms frame size, 240 bytes payload"),
             QStringLiteral("INFO"));

    // Pre-populate some latency history points
    double base = 28.0;
    for (int i = 0; i < 40; ++i) {
        double rtt = base + 4.0 * std::sin(i * 0.3) + (i == 15 ? 142.0 : 0.0);
        addLatencySample(rtt, base, i == 15);
    }
}

void VoiceDebugController::clearLogs()
{
    m_eventLogs.clear();
    emit eventLogsChanged();
}

void VoiceDebugController::pingNow()
{
    if (m_previewMode) {
        logEvent(QStringLiteral("PING"),
                 QStringLiteral("Manual ping triggered (Preview Ping: 29.4ms)"),
                 QStringLiteral("INFO"));
        addLatencySample(29.4, m_avgRtt, false);
        return;
    }

    if (m_udpPath) {
        auto peer = m_udpPath->firstPeer();
        if (peer) {
            m_udpPath->triggerPing(*peer);
        } else {
            logEvent(QStringLiteral("PING"),
                     QStringLiteral("Cannot ping: no active peer in UDP media path"),
                     QStringLiteral("WARN"));
        }
    }
}

void VoiceDebugController::setTransportMode(const QString &mode)
{
    if (m_transportSettings) {
        if (mode.compare(QStringLiteral("Udp"), Qt::CaseInsensitive) == 0)
            m_transportSettings->setTransportMode(TransportMode::Udp);
        else if (mode.compare(QStringLiteral("Tcp"), Qt::CaseInsensitive) == 0)
            m_transportSettings->setTransportMode(TransportMode::Tcp);
        else
            m_transportSettings->setTransportMode(TransportMode::Auto);

        logEvent(QStringLiteral("CONFIG"),
                 QStringLiteral("Transport setting changed to: %1").arg(mode),
                 QStringLiteral("INFO"));
    } else if (m_previewMode) {
        m_transportSetting = mode;
        if (mode == QStringLiteral("Tcp")) {
            m_carrierMode = QStringLiteral("WebSocket / TCP (Fallback)");
            m_peerUdpState = QStringLiteral("Suspended");
        } else {
            m_carrierMode = QStringLiteral("UDP (Relay-assisted)");
            m_peerUdpState = QStringLiteral("Active");
        }
        logEvent(QStringLiteral("CONFIG"),
                 QStringLiteral("Transport setting changed to: %1 (Preview)").arg(mode),
                 QStringLiteral("INFO"));
        emit telemetryChanged();
    }
}

void VoiceDebugController::simulateSpike(double spikeMs)
{
    m_lagSpikeCount++;
    m_isSpikeActive = true;
    m_currentRtt = spikeMs;
    m_maxRtt = std::max(m_maxRtt, spikeMs);
    addLatencySample(spikeMs, m_avgRtt > 0 ? m_avgRtt : 32.0, true);

    logEvent(QStringLiteral("SPIKE"),
             QStringLiteral("SIMULATED LAG SPIKE: %1 ms (baseline: %2 ms, delta: +%3 ms)")
                 .arg(spikeMs, 0, 'f', 1)
                 .arg(m_avgRtt, 0, 'f', 1)
                 .arg(spikeMs - m_avgRtt, 0, 'f', 1),
             QStringLiteral("SPIKE"));

    emit spikeAlert(spikeMs, m_avgRtt);
    emit telemetryChanged();
}

void VoiceDebugController::logEvent(const QString &category, const QString &message,
                                    const QString &severity)
{
    const QString timestamp = QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz"));
    const QString entry = QStringLiteral("[%1] [%2] [%3] %4")
                              .arg(timestamp, severity, category, message);

    m_eventLogs.prepend(entry);
    while (m_eventLogs.size() > 200)
        m_eventLogs.removeLast();

    emit eventLogsChanged();
}

void VoiceDebugController::addLatencySample(double rtt, double ema, bool isSpike)
{
    QVariantMap pt;
    pt[QStringLiteral("rtt")] = rtt;
    pt[QStringLiteral("ema")] = ema;
    pt[QStringLiteral("isSpike")] = isSpike;
    pt[QStringLiteral("time")] = QTime::currentTime().toString(QStringLiteral("hh:mm:ss"));

    m_latencyHistory.append(pt);
    while (m_latencyHistory.size() > 60)
        m_latencyHistory.removeFirst();

    emit latencyHistoryChanged();
}

void VoiceDebugController::onUdpDiagnosticEvent(const QString &category, const QString &message,
                                                const QString &severity)
{
    logEvent(category, message, severity);
}

void VoiceDebugController::onUdpLagSpike(const DeviceId &peer, double sampleMs, double avgMs)
{
    Q_UNUSED(peer);
    m_lagSpikeCount++;
    m_isSpikeActive = true;
    emit spikeAlert(sampleMs, avgMs);
}

void VoiceDebugController::onRefreshTick()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (m_previewMode) {
        m_previewTick++;

        // Base fluctuating RTT
        const double base = 28.0 + 5.0 * std::sin(m_previewTick * 0.12);
        double sample = base + (m_previewTick % 5 == 0 ? 3.5 : -2.0);

        // Inject periodic spike every 120 ticks (~12 seconds)
        bool spike = false;
        if (m_previewTick % 120 == 0) {
            sample = 168.0 + (m_previewTick % 25);
            spike = true;
            m_lagSpikeCount++;
            m_isSpikeActive = true;
            logEvent(QStringLiteral("SPIKE"),
                     QStringLiteral("LATENCY SPIKE DETECTED: %1 ms (baseline: %2 ms, delta: +%3 ms)")
                         .arg(sample, 0, 'f', 1)
                         .arg(m_avgRtt, 0, 'f', 1)
                         .arg(sample - m_avgRtt, 0, 'f', 1),
                     QStringLiteral("SPIKE"));
            emit spikeAlert(sample, m_avgRtt);
        } else if (m_previewTick % 120 > 5) {
            m_isSpikeActive = false;
        }

        m_currentRtt = sample;
        if (m_avgRtt == 0.0) {
            m_avgRtt = sample;
            m_minRtt = sample;
            m_maxRtt = sample;
        } else {
            m_avgRtt = 0.9 * m_avgRtt + 0.1 * sample;
            m_minRtt = std::min(m_minRtt, sample);
            m_maxRtt = std::max(m_maxRtt, sample);
        }

        // Add history point every 3 ticks (~300ms) or on spike
        if (spike || (m_previewTick % 3 == 0)) {
            addLatencySample(sample, m_avgRtt, spike);
        }

        // Counters update
        m_framesCaptured += 5;
        m_framesSent += 5;
        m_framesPlayed += 5;
        m_packetsReceived += 5;
        m_bytesSent += 1150;
        m_bytesReceived += 1150;
        m_pingsSent = m_previewTick / 50 + 1;
        m_pongsReceived = m_pingsSent;
        m_unackedPings = 0;

        // Bitrates
        m_sendBitrateKbps = 92.0 + 4.0 * std::sin(m_previewTick * 0.2);
        m_recvBitrateKbps = 91.5 + 3.0 * std::cos(m_previewTick * 0.2);

        // Duration
        m_callDuration = formatDuration(m_previewTick * 100);

        // Audio levels
        if (m_previewCallCtrl) {
            m_localAudioLevel = m_previewCallCtrl->localLevel();
            m_remoteAudioLevel = m_previewCallCtrl->remoteLevel();
            m_localSpeaking = m_previewCallCtrl->localSpeaking();
            m_remoteSpeaking = m_previewCallCtrl->remoteSpeaking();
            m_muted = m_previewCallCtrl->muted();
        } else {
            m_remoteSpeaking = (m_previewTick % 40) < 25;
            m_remoteAudioLevel = m_remoteSpeaking ? 0.65 + 0.2 * std::sin(m_previewTick * 0.4) : 0.02;
            m_localSpeaking = (m_previewTick % 70) < 15;
            m_localAudioLevel = m_localSpeaking ? 0.45 + 0.15 * std::cos(m_previewTick * 0.4) : 0.01;
        }
        m_micGateOpen = m_localSpeaking || (m_localAudioLevel > 0.05);

        emit telemetryChanged();
        return;
    }

    // Live mode updates
    if (m_callEngine) {
        m_hasCall = (m_callEngine->state() != CallState::Idle);
        m_callState = callStateName(m_callEngine->state());
        m_callDirection = m_callEngine->direction() == CallDirection::Incoming
            ? QStringLiteral("Incoming")
            : QStringLiteral("Outgoing");

        if (auto cid = m_callEngine->currentCallId())
            m_callId = cid->toHex().left(16).toUpper();
        else
            m_callId = QStringLiteral("--");

        m_callDuration = formatDuration(m_callEngine->activeDurationMs());
        m_codecName = m_callEngine->currentCodec() == AudioCodecKind::Opus
            ? QStringLiteral("Opus 48kHz (Mono, 20ms frames)")
            : QStringLiteral("PCM 48kHz (Mono, 20ms frames)");

        m_localAudioLevel = m_callEngine->localLevel();
        m_remoteAudioLevel = m_callEngine->remoteLevel();
        m_localSpeaking = m_callEngine->isLocalSpeaking();
        m_remoteSpeaking = m_callEngine->isRemoteSpeaking();
        m_micGateOpen = m_callEngine->isMicrophoneGateOpen();
        m_muted = m_callEngine->isMuted();

        if (auto stats = m_callEngine->sessionStats()) {
            m_framesCaptured = stats->framesCaptured;
            m_framesSent = stats->framesSent;
            m_framesPlayed = stats->framesPlayed;
            m_framesConcealed = stats->framesConcealed;
            m_framesSilent = stats->framesSilent;
            m_packetsReceived = stats->packetsReceived;
            m_packetsRejected = stats->packetsRejected;
            m_bytesSent = stats->bytesSent;
            m_bytesReceived = stats->bytesReceived;

            if (m_packetsReceived + stats->framesConcealed > 0) {
                m_lossRatePercent = 100.0 * static_cast<double>(stats->framesConcealed)
                    / static_cast<double>(m_packetsReceived + stats->framesConcealed);
            }
        }

        if (auto jstats = m_callEngine->jitterStats()) {
            m_jitterMs = static_cast<int>(jstats->jitterMs);
            m_targetDepth = jstats->targetDepth;
            m_peakDepth = jstats->peakDepth;
            m_bufferStarved = jstats->starved;
            m_bufferLost = jstats->lost;
            m_bufferLate = jstats->late;
            m_bufferDuplicates = jstats->duplicates;
            m_bufferOverflows = jstats->overflows;
            m_bufferResets = jstats->resets;
            m_bufferInserted = jstats->inserted;
            m_bufferDropped = jstats->dropped;
            m_bufferAccepted = jstats->accepted;
            m_bufferPlayed = jstats->played;
        }
    }

    if (m_micSettings) {
        m_micGain = m_micSettings->gain();
        m_gateThresholdDb = m_micSettings->noiseGateThresholdDb();
    }

    if (m_transportSettings) {
        switch (m_transportSettings->transportMode()) {
        case TransportMode::Auto:
            m_transportSetting = QStringLiteral("Auto");
            break;
        case TransportMode::Udp:
            m_transportSetting = QStringLiteral("Udp");
            break;
        case TransportMode::Tcp:
            m_transportSetting = QStringLiteral("Tcp");
            break;
        }
    }

    if (m_udpPath) {
        m_relayEndpoint = QStringLiteral("%1:%2")
                              .arg(m_udpPath->relayAddress().toString())
                              .arg(m_udpPath->relayPort());
        m_localPort = m_udpPath->localPort();

        auto peer = m_udpPath->firstPeer();
        if (peer) {
            auto telem = m_udpPath->peerTelemetry(*peer);
            if (telem) {
                m_peerUdpState = peerStateName(telem->state);
                if (m_transportSettings && m_transportSettings->transportMode() == TransportMode::Tcp) {
                    m_carrierMode = QStringLiteral("TCP Only (Forced)");
                } else if (telem->state == UdpPeerState::Active) {
                    m_carrierMode = QStringLiteral("UDP (Relay-assisted)");
                } else {
                    m_carrierMode = QStringLiteral("WebSocket / TCP (Fallback)");
                }

                m_currentRtt = telem->lastRawRttMs;
                m_avgRtt = telem->rttMs;
                m_minRtt = telem->minRttMs;
                m_maxRtt = telem->maxRttMs;
                m_lagSpikeCount = telem->lagSpikeCount;
                m_silenceMs = static_cast<int>(telem->silenceMs);
                m_silencePercent = std::min(1.0, telem->silenceMs / 3000.0);
                m_pingsSent = telem->pingsSent;
                m_pongsReceived = telem->pongsReceived;
                m_unackedPings = telem->unackedPings;

                m_isSpikeActive = (telem->lastRawRttMs > 100.0
                                   && telem->lastRawRttMs > 1.5 * telem->rttMs);

                // Sample history
                if (telem->lastRawRttMs > 0.0) {
                    addLatencySample(telem->lastRawRttMs, telem->rttMs, m_isSpikeActive);
                }
            }
        } else {
            m_carrierMode = QStringLiteral("UDP Ready (Awaiting Call)");
            m_peerUdpState = QStringLiteral("Inactive");
        }
    }

    // Calculate bitrates
    if (m_lastBitrateCalcTimeMs > 0) {
        const qint64 dt = nowMs - m_lastBitrateCalcTimeMs;
        if (dt >= 500) {
            const quint64 dBytesSent = m_bytesSent >= m_lastBytesSent
                ? m_bytesSent - m_lastBytesSent
                : 0;
            const quint64 dBytesRecv = m_bytesReceived >= m_lastBytesReceived
                ? m_bytesReceived - m_lastBytesReceived
                : 0;

            m_sendBitrateKbps = (dBytesSent * 8.0) / static_cast<double>(dt);
            m_recvBitrateKbps = (dBytesRecv * 8.0) / static_cast<double>(dt);

            m_lastBytesSent = m_bytesSent;
            m_lastBytesReceived = m_bytesReceived;
            m_lastBitrateCalcTimeMs = nowMs;
        }
    } else {
        m_lastBytesSent = m_bytesSent;
        m_lastBytesReceived = m_bytesReceived;
        m_lastBitrateCalcTimeMs = nowMs;
    }

    emit telemetryChanged();
}

} // namespace OpenChat
