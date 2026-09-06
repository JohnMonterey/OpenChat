#pragma once

#include "call/QtAudioIo.h"
#include "media/MicrophoneProcessor.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QObject>
#include <QString>
#include <QVariantList>

#include <memory>

namespace OpenChat {

// The microphone as the user configures it: which device, how loud, and how
// the noise gate behaves. Persisted in QSettings and exposed to QML as the
// Audio & Video → Microphone panel.
//
// Also runs the "test your microphone" meter: while testing, it opens the
// chosen device through the same capture path a call uses and pushes every
// frame through the same MicrophoneProcessor, so the level bar and the gate
// light in settings show exactly what a call would send.
//
// One instance serves the whole process. main() creates it so the call engine
// can be wired to it; the QML singleton registration hands out that same
// object, and creates one only when nothing else has (the QML tests).
class MicrophoneSettings final : public QObject
{
    Q_OBJECT
    // Every input the system offers: a list of {id, name, isDefault} maps,
    // refreshed whenever a device comes or goes.
    Q_PROPERTY(QVariantList inputDevices READ inputDevices NOTIFY inputDevicesChanged)
    // The chosen device's id; empty follows the system default.
    Q_PROPERTY(QString inputDeviceId READ inputDeviceId WRITE setInputDeviceId NOTIFY
                   inputDeviceChanged)
    Q_PROPERTY(QString inputDeviceName READ inputDeviceName NOTIFY inputDeviceChanged)
    // Linear gain, 0.0 to 2.0. 1.0 is the device as it comes.
    Q_PROPERTY(double gain READ gain WRITE setGain NOTIFY processingChanged)
    Q_PROPERTY(bool noiseGateEnabled READ noiseGateEnabled WRITE setNoiseGateEnabled NOTIFY
                   processingChanged)
    // Gate threshold in dBFS, minThresholdDb to maxThresholdDb. Lower opens
    // the gate for quieter sounds.
    Q_PROPERTY(double noiseGateThresholdDb READ noiseGateThresholdDb WRITE
                   setNoiseGateThresholdDb NOTIFY processingChanged)
    Q_PROPERTY(double minThresholdDb READ minThresholdDb CONSTANT)
    Q_PROPERTY(double maxThresholdDb READ maxThresholdDb CONSTANT)

    // The live test meter.
    Q_PROPERTY(bool testing READ isTesting NOTIFY testingChanged)
    Q_PROPERTY(QString testError READ testError NOTIFY testingChanged)
    // Smoothed level after gain, 0 to 1 linear, and the same in dBFS.
    Q_PROPERTY(double level READ level NOTIFY meterChanged)
    Q_PROPERTY(double levelDb READ levelDb NOTIFY meterChanged)
    Q_PROPERTY(bool gateOpen READ isGateOpen NOTIFY meterChanged)

public:
    explicit MicrophoneSettings(QObject *parent = nullptr);
    ~MicrophoneSettings() override;

    // The process-wide instance, or nullptr before one exists.
    [[nodiscard]] static MicrophoneSettings *instance();

    [[nodiscard]] QVariantList inputDevices() const;
    [[nodiscard]] QString inputDeviceId() const { return m_inputDeviceId; }
    [[nodiscard]] QString inputDeviceName() const;
    void setInputDeviceId(const QString &id);
    // The device a call should open right now, as a chooser for the factory.
    [[nodiscard]] QAudioDevice selectedInputDevice() const;
    [[nodiscard]] AudioInputChooser inputChooser() const;

    [[nodiscard]] double gain() const { return m_processing.gain; }
    void setGain(double gain);
    [[nodiscard]] bool noiseGateEnabled() const { return m_processing.gateEnabled; }
    void setNoiseGateEnabled(bool enabled);
    [[nodiscard]] double noiseGateThresholdDb() const;
    void setNoiseGateThresholdDb(double db);
    [[nodiscard]] static constexpr double minThresholdDb() { return -60.0; }
    [[nodiscard]] static constexpr double maxThresholdDb() { return -10.0; }

    // The gain and gate exactly as the call engine should apply them.
    [[nodiscard]] const MicrophoneProcessor::Config &processing() const { return m_processing; }

    [[nodiscard]] bool isTesting() const { return m_capture != nullptr; }
    [[nodiscard]] QString testError() const { return m_testError; }
    [[nodiscard]] double level() const { return m_level; }
    [[nodiscard]] double levelDb() const;
    [[nodiscard]] bool isGateOpen() const { return m_gateOpen; }

    Q_INVOKABLE void startTest();
    Q_INVOKABLE void stopTest();
    Q_INVOKABLE void resetToDefaults();

    // Conversions shared with the QML sliders.
    [[nodiscard]] static double dbToLinear(double db);
    [[nodiscard]] static double linearToDb(double linear);

signals:
    void inputDevicesChanged();
    void inputDeviceChanged();
    // Gain or gate changed: the engine should be handed processing() again.
    void processingChanged();
    void testingChanged();
    void meterChanged();

private:
    void load();
    void save() const;
    void onTestFrame(const AudioFrame &frame);

    QMediaDevices m_devices;
    QString m_inputDeviceId;
    MicrophoneProcessor::Config m_processing;

    std::unique_ptr<QtAudioCaptureSource> m_capture;
    MicrophoneProcessor m_monitor;
    QString m_testError;
    double m_level = 0.0;
    bool m_gateOpen = true;
};

} // namespace OpenChat
