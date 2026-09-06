#include "app/MicrophoneSettings.h"

#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {

constexpr auto keyInputDevice = "Audio/inputDevice";
constexpr auto keyGain = "Audio/inputGain";
constexpr auto keyGateEnabled = "Audio/noiseGate";
constexpr auto keyGateThresholdDb = "Audio/noiseGateThresholdDb";

constexpr double maxGain = 2.0;

MicrophoneSettings *s_instance = nullptr;

} // namespace

MicrophoneSettings::MicrophoneSettings(QObject *parent)
    : QObject(parent)
{
    if (!s_instance)
        s_instance = this;
    load();
    connect(&m_devices, &QMediaDevices::audioInputsChanged, this, [this] {
        emit inputDevicesChanged();
        // The chosen device may have just arrived or gone; the name shown
        // beside the picker follows either way.
        emit inputDeviceChanged();
    });
}

MicrophoneSettings::~MicrophoneSettings()
{
    stopTest();
    if (s_instance == this)
        s_instance = nullptr;
}

MicrophoneSettings *MicrophoneSettings::instance()
{
    return s_instance;
}

void MicrophoneSettings::load()
{
    const QSettings settings;
    m_inputDeviceId = settings.value(QLatin1String(keyInputDevice)).toString();
    const MicrophoneProcessor::Config defaults;
    m_processing.gain = std::clamp(
        settings.value(QLatin1String(keyGain), defaults.gain).toDouble(), 0.0, maxGain);
    m_processing.gateEnabled =
        settings.value(QLatin1String(keyGateEnabled), defaults.gateEnabled).toBool();
    const double db = settings.value(QLatin1String(keyGateThresholdDb),
                                     linearToDb(defaults.gateThreshold))
                          .toDouble();
    m_processing.gateThreshold = dbToLinear(std::clamp(db, minThresholdDb(), maxThresholdDb()));
    m_processing.gateHoldFrames = defaults.gateHoldFrames;
    m_monitor.setConfig(m_processing);
}

void MicrophoneSettings::save() const
{
    QSettings settings;
    if (m_inputDeviceId.isEmpty())
        settings.remove(QLatin1String(keyInputDevice));
    else
        settings.setValue(QLatin1String(keyInputDevice), m_inputDeviceId);
    settings.setValue(QLatin1String(keyGain), m_processing.gain);
    settings.setValue(QLatin1String(keyGateEnabled), m_processing.gateEnabled);
    settings.setValue(QLatin1String(keyGateThresholdDb), noiseGateThresholdDb());
    settings.sync();
}

QVariantList MicrophoneSettings::inputDevices() const
{
    QVariantList list;
    const QAudioDevice systemDefault = QMediaDevices::defaultAudioInput();
    for (const QAudioDevice &device : QMediaDevices::audioInputs()) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), QString::fromUtf8(device.id()));
        row.insert(QStringLiteral("name"), device.description());
        row.insert(QStringLiteral("isDefault"), device == systemDefault);
        list.append(row);
    }
    return list;
}

QString MicrophoneSettings::inputDeviceName() const
{
    const QAudioDevice device = selectedInputDevice();
    if (device.isNull())
        return QStringLiteral("No microphone found");
    if (m_inputDeviceId.isEmpty())
        return QStringLiteral("System default (%1)").arg(device.description());
    return device.description();
}

void MicrophoneSettings::setInputDeviceId(const QString &id)
{
    if (id == m_inputDeviceId)
        return;
    m_inputDeviceId = id;
    save();
    emit inputDeviceChanged();
    // A running test follows the choice, so the bar shows the new device.
    if (isTesting()) {
        stopTest();
        startTest();
    }
}

QAudioDevice MicrophoneSettings::selectedInputDevice() const
{
    if (!m_inputDeviceId.isEmpty()) {
        const QByteArray wanted = m_inputDeviceId.toUtf8();
        for (const QAudioDevice &device : QMediaDevices::audioInputs())
            if (device.id() == wanted)
                return device;
    }
    return QMediaDevices::defaultAudioInput();
}

AudioInputChooser MicrophoneSettings::inputChooser() const
{
    // Captures the id, not the device: the chooser is consulted when a call
    // starts, and the device list may have changed since it was built.
    return [id = m_inputDeviceId] {
        if (id.isEmpty())
            return QAudioDevice();
        const QByteArray wanted = id.toUtf8();
        for (const QAudioDevice &device : QMediaDevices::audioInputs())
            if (device.id() == wanted)
                return device;
        return QAudioDevice();
    };
}

void MicrophoneSettings::setGain(double gain)
{
    gain = std::clamp(std::isfinite(gain) ? gain : 1.0, 0.0, maxGain);
    if (qFuzzyCompare(gain, m_processing.gain))
        return;
    m_processing.gain = gain;
    m_monitor.setConfig(m_processing);
    save();
    emit processingChanged();
}

void MicrophoneSettings::setNoiseGateEnabled(bool enabled)
{
    if (enabled == m_processing.gateEnabled)
        return;
    m_processing.gateEnabled = enabled;
    m_monitor.setConfig(m_processing);
    save();
    emit processingChanged();
}

double MicrophoneSettings::noiseGateThresholdDb() const
{
    return std::clamp(linearToDb(m_processing.gateThreshold), minThresholdDb(), maxThresholdDb());
}

void MicrophoneSettings::setNoiseGateThresholdDb(double db)
{
    db = std::clamp(std::isfinite(db) ? db : linearToDb(MicrophoneProcessor::Config{}.gateThreshold),
                    minThresholdDb(), maxThresholdDb());
    if (qFuzzyCompare(db, noiseGateThresholdDb()))
        return;
    m_processing.gateThreshold = dbToLinear(db);
    m_monitor.setConfig(m_processing);
    save();
    emit processingChanged();
}

double MicrophoneSettings::levelDb() const
{
    return std::max(linearToDb(m_level), -90.0);
}

void MicrophoneSettings::startTest()
{
    if (m_capture)
        return;
    m_testError.clear();
    m_monitor.reset();
    m_level = 0.0;
    m_gateOpen = true;
    auto capture = std::make_unique<QtAudioCaptureSource>(inputChooser());
    capture->onFrame = [this](const AudioFrame &frame) { onTestFrame(frame); };
    if (!capture->start()) {
        m_testError = selectedInputDevice().isNull()
            ? QStringLiteral("No microphone was found.")
            : QStringLiteral("The microphone could not be opened.");
        emit testingChanged();
        emit meterChanged();
        return;
    }
    m_capture = std::move(capture);
    emit testingChanged();
    emit meterChanged();
}

void MicrophoneSettings::stopTest()
{
    if (!m_capture)
        return;
    m_capture->onFrame = nullptr;
    m_capture->stop();
    m_capture.reset();
    m_level = 0.0;
    m_gateOpen = true;
    emit testingChanged();
    emit meterChanged();
}

void MicrophoneSettings::resetToDefaults()
{
    const MicrophoneProcessor::Config defaults;
    setInputDeviceId(QString());
    setGain(defaults.gain);
    setNoiseGateEnabled(defaults.gateEnabled);
    setNoiseGateThresholdDb(linearToDb(defaults.gateThreshold));
}

void MicrophoneSettings::onTestFrame(const AudioFrame &frame)
{
    (void)m_monitor.process(frame);
    const double level = std::clamp(m_monitor.level(), 0.0, 1.0);
    const bool open = m_monitor.isGateOpen();
    if (qFuzzyCompare(level + 1.0, m_level + 1.0) && open == m_gateOpen)
        return;
    m_level = level;
    m_gateOpen = open;
    emit meterChanged();
}

double MicrophoneSettings::dbToLinear(double db)
{
    return std::pow(10.0, db / 20.0);
}

double MicrophoneSettings::linearToDb(double linear)
{
    if (linear <= 0.0)
        return -120.0;
    return 20.0 * std::log10(linear);
}

} // namespace OpenChat
