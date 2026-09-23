#pragma once

#include "domain/Identifiers.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <memory>
#include <optional>

namespace OpenChat {

class CallController;
class CallEngine;
class MicrophoneSettings;
class TransportSettings;
class UdpCallMediaPath;

class VoiceDebugController final : public QObject
{
    Q_OBJECT

    // Call state & metadata
    Q_PROPERTY(bool hasCall READ hasCall NOTIFY telemetryChanged)
    Q_PROPERTY(QString callId READ callId NOTIFY telemetryChanged)
    Q_PROPERTY(QString callState READ callState NOTIFY telemetryChanged)
    Q_PROPERTY(QString callDirection READ callDirection NOTIFY telemetryChanged)
    Q_PROPERTY(QString callDuration READ callDuration NOTIFY telemetryChanged)
    Q_PROPERTY(QString codecName READ codecName NOTIFY telemetryChanged)

    // Carrier & Transport
    Q_PROPERTY(QString carrierMode READ carrierMode NOTIFY telemetryChanged)
    Q_PROPERTY(QString peerUdpState READ peerUdpState NOTIFY telemetryChanged)
    Q_PROPERTY(QString relayEndpoint READ relayEndpoint NOTIFY telemetryChanged)
    Q_PROPERTY(int localPort READ localPort NOTIFY telemetryChanged)
    Q_PROPERTY(int silenceMs READ silenceMs NOTIFY telemetryChanged)
    Q_PROPERTY(int silenceLimitMs READ silenceLimitMs CONSTANT)
    Q_PROPERTY(double silencePercent READ silencePercent NOTIFY telemetryChanged)
    Q_PROPERTY(QString transportSetting READ transportSetting NOTIFY telemetryChanged)

    // Latency & Spikes
    Q_PROPERTY(double currentRtt READ currentRtt NOTIFY telemetryChanged)
    Q_PROPERTY(double avgRtt READ avgRtt NOTIFY telemetryChanged)
    Q_PROPERTY(double minRtt READ minRtt NOTIFY telemetryChanged)
    Q_PROPERTY(double maxRtt READ maxRtt NOTIFY telemetryChanged)
    Q_PROPERTY(int lagSpikeCount READ lagSpikeCount NOTIFY telemetryChanged)
    Q_PROPERTY(bool isSpikeActive READ isSpikeActive NOTIFY telemetryChanged)
    Q_PROPERTY(QVariantList latencyHistory READ latencyHistory NOTIFY latencyHistoryChanged)

    // Jitter Buffer & Underrun
    Q_PROPERTY(int jitterMs READ jitterMs NOTIFY telemetryChanged)
    Q_PROPERTY(int targetDepth READ targetDepth NOTIFY telemetryChanged)
    Q_PROPERTY(int targetDepthMs READ targetDepthMs NOTIFY telemetryChanged)
    Q_PROPERTY(int peakDepth READ peakDepth NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferStarved READ bufferStarved NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferLost READ bufferLost NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferLate READ bufferLate NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferDuplicates READ bufferDuplicates NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferOverflows READ bufferOverflows NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferResets READ bufferResets NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferInserted READ bufferInserted NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferDropped READ bufferDropped NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferAccepted READ bufferAccepted NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bufferPlayed READ bufferPlayed NOTIFY telemetryChanged)

    // Audio stream & PLC
    Q_PROPERTY(quint64 framesCaptured READ framesCaptured NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 framesSent READ framesSent NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 framesPlayed READ framesPlayed NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 framesConcealed READ framesConcealed NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 framesSilent READ framesSilent NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 packetsReceived READ packetsReceived NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 packetsRejected READ packetsRejected NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bytesSent READ bytesSent NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 bytesReceived READ bytesReceived NOTIFY telemetryChanged)
    Q_PROPERTY(double sendBitrateKbps READ sendBitrateKbps NOTIFY telemetryChanged)
    Q_PROPERTY(double recvBitrateKbps READ recvBitrateKbps NOTIFY telemetryChanged)
    Q_PROPERTY(double lossRatePercent READ lossRatePercent NOTIFY telemetryChanged)

    // Audio hardware & gate
    Q_PROPERTY(double localAudioLevel READ localAudioLevel NOTIFY telemetryChanged)
    Q_PROPERTY(double remoteAudioLevel READ remoteAudioLevel NOTIFY telemetryChanged)
    Q_PROPERTY(bool localSpeaking READ localSpeaking NOTIFY telemetryChanged)
    Q_PROPERTY(bool remoteSpeaking READ remoteSpeaking NOTIFY telemetryChanged)
    Q_PROPERTY(bool micGateOpen READ micGateOpen NOTIFY telemetryChanged)
    Q_PROPERTY(double micGain READ micGain NOTIFY telemetryChanged)
    Q_PROPERTY(double gateThresholdDb READ gateThresholdDb NOTIFY telemetryChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY telemetryChanged)

    // Ping / Heartbeat stats
    Q_PROPERTY(quint64 pingsSent READ pingsSent NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 pongsReceived READ pongsReceived NOTIFY telemetryChanged)
    Q_PROPERTY(quint64 unackedPings READ unackedPings NOTIFY telemetryChanged)

    // Event log
    Q_PROPERTY(QStringList eventLogs READ eventLogs NOTIFY eventLogsChanged)

public:
    explicit VoiceDebugController(QObject *parent = nullptr);
    ~VoiceDebugController() override;

    void setLiveSources(CallEngine *engine, UdpCallMediaPath *udpPath,
                        TransportSettings *transportSettings,
                        MicrophoneSettings *micSettings);
    void enableForPreview(CallController *previewCallCtrl = nullptr);

    [[nodiscard]] bool hasCall() const noexcept { return m_hasCall; }
    [[nodiscard]] QString callId() const { return m_callId; }
    [[nodiscard]] QString callState() const { return m_callState; }
    [[nodiscard]] QString callDirection() const { return m_callDirection; }
    [[nodiscard]] QString callDuration() const { return m_callDuration; }
    [[nodiscard]] QString codecName() const { return m_codecName; }

    [[nodiscard]] QString carrierMode() const { return m_carrierMode; }
    [[nodiscard]] QString peerUdpState() const { return m_peerUdpState; }
    [[nodiscard]] QString relayEndpoint() const { return m_relayEndpoint; }
    [[nodiscard]] int localPort() const noexcept { return m_localPort; }
    [[nodiscard]] int silenceMs() const noexcept { return m_silenceMs; }
    [[nodiscard]] constexpr int silenceLimitMs() const noexcept { return 3000; }
    [[nodiscard]] double silencePercent() const noexcept { return m_silencePercent; }
    [[nodiscard]] QString transportSetting() const { return m_transportSetting; }

    [[nodiscard]] double currentRtt() const noexcept { return m_currentRtt; }
    [[nodiscard]] double avgRtt() const noexcept { return m_avgRtt; }
    [[nodiscard]] double minRtt() const noexcept { return m_minRtt; }
    [[nodiscard]] double maxRtt() const noexcept { return m_maxRtt; }
    [[nodiscard]] int lagSpikeCount() const noexcept { return m_lagSpikeCount; }
    [[nodiscard]] bool isSpikeActive() const noexcept { return m_isSpikeActive; }
    [[nodiscard]] QVariantList latencyHistory() const { return m_latencyHistory; }

    [[nodiscard]] int jitterMs() const noexcept { return m_jitterMs; }
    [[nodiscard]] int targetDepth() const noexcept { return m_targetDepth; }
    [[nodiscard]] int targetDepthMs() const noexcept { return m_targetDepth * 20; }
    [[nodiscard]] int peakDepth() const noexcept { return m_peakDepth; }
    [[nodiscard]] quint64 bufferStarved() const noexcept { return m_bufferStarved; }
    [[nodiscard]] quint64 bufferLost() const noexcept { return m_bufferLost; }
    [[nodiscard]] quint64 bufferLate() const noexcept { return m_bufferLate; }
    [[nodiscard]] quint64 bufferDuplicates() const noexcept { return m_bufferDuplicates; }
    [[nodiscard]] quint64 bufferOverflows() const noexcept { return m_bufferOverflows; }
    [[nodiscard]] quint64 bufferResets() const noexcept { return m_bufferResets; }
    [[nodiscard]] quint64 bufferInserted() const noexcept { return m_bufferInserted; }
    [[nodiscard]] quint64 bufferDropped() const noexcept { return m_bufferDropped; }
    [[nodiscard]] quint64 bufferAccepted() const noexcept { return m_bufferAccepted; }
    [[nodiscard]] quint64 bufferPlayed() const noexcept { return m_bufferPlayed; }

    [[nodiscard]] quint64 framesCaptured() const noexcept { return m_framesCaptured; }
    [[nodiscard]] quint64 framesSent() const noexcept { return m_framesSent; }
    [[nodiscard]] quint64 framesPlayed() const noexcept { return m_framesPlayed; }
    [[nodiscard]] quint64 framesConcealed() const noexcept { return m_framesConcealed; }
    [[nodiscard]] quint64 framesSilent() const noexcept { return m_framesSilent; }
    [[nodiscard]] quint64 packetsReceived() const noexcept { return m_packetsReceived; }
    [[nodiscard]] quint64 packetsRejected() const noexcept { return m_packetsRejected; }
    [[nodiscard]] quint64 bytesSent() const noexcept { return m_bytesSent; }
    [[nodiscard]] quint64 bytesReceived() const noexcept { return m_bytesReceived; }
    [[nodiscard]] double sendBitrateKbps() const noexcept { return m_sendBitrateKbps; }
    [[nodiscard]] double recvBitrateKbps() const noexcept { return m_recvBitrateKbps; }
    [[nodiscard]] double lossRatePercent() const noexcept { return m_lossRatePercent; }

    [[nodiscard]] double localAudioLevel() const noexcept { return m_localAudioLevel; }
    [[nodiscard]] double remoteAudioLevel() const noexcept { return m_remoteAudioLevel; }
    [[nodiscard]] bool localSpeaking() const noexcept { return m_localSpeaking; }
    [[nodiscard]] bool remoteSpeaking() const noexcept { return m_remoteSpeaking; }
    [[nodiscard]] bool micGateOpen() const noexcept { return m_micGateOpen; }
    [[nodiscard]] double micGain() const noexcept { return m_micGain; }
    [[nodiscard]] double gateThresholdDb() const noexcept { return m_gateThresholdDb; }
    [[nodiscard]] bool muted() const noexcept { return m_muted; }

    [[nodiscard]] quint64 pingsSent() const noexcept { return m_pingsSent; }
    [[nodiscard]] quint64 pongsReceived() const noexcept { return m_pongsReceived; }
    [[nodiscard]] quint64 unackedPings() const noexcept { return m_unackedPings; }

    [[nodiscard]] QStringList eventLogs() const { return m_eventLogs; }

    Q_INVOKABLE void clearLogs();
    Q_INVOKABLE void pingNow();
    Q_INVOKABLE void setTransportMode(const QString &mode);
    Q_INVOKABLE void simulateSpike(double spikeMs = 180.0);

signals:
    void telemetryChanged();
    void latencyHistoryChanged();
    void eventLogsChanged();
    void spikeAlert(double sampleMs, double avgMs);

private slots:
    void onRefreshTick();
    void onUdpDiagnosticEvent(const QString &category, const QString &message,
                              const QString &severity);
    void onUdpLagSpike(const DeviceId &peer, double sampleMs, double avgMs);

private:
    void logEvent(const QString &category, const QString &message,
                  const QString &severity = QStringLiteral("INFO"));
    void addLatencySample(double rtt, double ema, bool isSpike);

    CallEngine *m_callEngine = nullptr;
    UdpCallMediaPath *m_udpPath = nullptr;
    TransportSettings *m_transportSettings = nullptr;
    MicrophoneSettings *m_micSettings = nullptr;
    CallController *m_previewCallCtrl = nullptr;

    QTimer *m_timer = nullptr;
    bool m_previewMode = false;
    quint64 m_previewTick = 0;

    // Bitrate tracking
    quint64 m_lastBytesSent = 0;
    quint64 m_lastBytesReceived = 0;
    qint64 m_lastBitrateCalcTimeMs = 0;

    // Call state
    bool m_hasCall = false;
    QString m_callId = QStringLiteral("--");
    QString m_callState = QStringLiteral("Idle");
    QString m_callDirection = QStringLiteral("None");
    QString m_callDuration = QStringLiteral("00:00");
    QString m_codecName = QStringLiteral("Opus 48kHz (Mono, 20ms)");

    // Carrier
    QString m_carrierMode = QStringLiteral("UDP Ready");
    QString m_peerUdpState = QStringLiteral("Inactive");
    QString m_relayEndpoint = QStringLiteral("127.0.0.1:8444");
    int m_localPort = 0;
    int m_silenceMs = 0;
    double m_silencePercent = 0.0;
    QString m_transportSetting = QStringLiteral("Auto");

    // Latency
    double m_currentRtt = 0.0;
    double m_avgRtt = 0.0;
    double m_minRtt = 0.0;
    double m_maxRtt = 0.0;
    int m_lagSpikeCount = 0;
    bool m_isSpikeActive = false;
    QVariantList m_latencyHistory;

    // Jitter
    int m_jitterMs = 0;
    int m_targetDepth = 0;
    int m_peakDepth = 0;
    quint64 m_bufferStarved = 0;
    quint64 m_bufferLost = 0;
    quint64 m_bufferLate = 0;
    quint64 m_bufferDuplicates = 0;
    quint64 m_bufferOverflows = 0;
    quint64 m_bufferResets = 0;
    quint64 m_bufferInserted = 0;
    quint64 m_bufferDropped = 0;
    quint64 m_bufferAccepted = 0;
    quint64 m_bufferPlayed = 0;

    // Audio stream
    quint64 m_framesCaptured = 0;
    quint64 m_framesSent = 0;
    quint64 m_framesPlayed = 0;
    quint64 m_framesConcealed = 0;
    quint64 m_framesSilent = 0;
    quint64 m_packetsReceived = 0;
    quint64 m_packetsRejected = 0;
    quint64 m_bytesSent = 0;
    quint64 m_bytesReceived = 0;
    double m_sendBitrateKbps = 0.0;
    double m_recvBitrateKbps = 0.0;
    double m_lossRatePercent = 0.0;

    // Audio hardware
    double m_localAudioLevel = 0.0;
    double m_remoteAudioLevel = 0.0;
    bool m_localSpeaking = false;
    bool m_remoteSpeaking = false;
    bool m_micGateOpen = true;
    double m_micGain = 1.0;
    double m_gateThresholdDb = -45.0;
    bool m_muted = false;

    // Pings
    quint64 m_pingsSent = 0;
    quint64 m_pongsReceived = 0;
    quint64 m_unackedPings = 0;

    // Events
    QStringList m_eventLogs;
};

} // namespace OpenChat
