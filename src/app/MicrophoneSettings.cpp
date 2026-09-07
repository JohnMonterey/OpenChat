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
constexpr auto keyVoiceEffect = "Audio/voiceEffect";
constexpr auto keyEffectIntensity = "Audio/effectIntensity";
constexpr auto keyStudio = "Audio/studioVoice";
constexpr auto keyNoiseReduction = "Audio/noiseReduction";
constexpr auto keyAutomaticGain = "Audio/automaticGain";
constexpr auto keyCompressor = "Audio/compressor";
struct EffectEntry { const char *id; const char *name; };
// Stable persisted ids, in VoiceEffects::Effect order.
constexpr EffectEntry effects[] = {
    {"none", "None"}, {"radio", "Radio"}, {"walkie-talkie", "Walkie-Talkie"},
    {"telephone", "Telephone"}, {"deep", "Deep Voice"}, {"tiny", "Tiny Voice"},
    {"robot", "Robot"}, {"intercom", "Intercom"}, {"cave", "Cave"},
    {"bathroom", "Bathroom"}, {"megaphone", "Megaphone"}, {"anonymous", "Anonymous"}
};

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
    const auto effectId = settings.value(QLatin1String(keyVoiceEffect)).toString();
    for (int i = 0; i < int(std::size(effects)); ++i)
        if (effectId == QLatin1String(effects[i].id))
            m_processing.voice.effect = static_cast<VoiceEffects::Effect>(i);
    const double intensity = settings.value(QLatin1String(keyEffectIntensity), 0.5).toDouble();
    m_processing.voice.intensity = std::clamp(std::isfinite(intensity) ? intensity : 0.5, 0.0, 1.0);
    m_processing.voice.studio = settings.value(QLatin1String(keyStudio), false).toBool();
    m_processing.voice.noiseReduction = settings.value(QLatin1String(keyNoiseReduction), false).toBool();
    m_processing.voice.automaticGain = settings.value(QLatin1String(keyAutomaticGain), false).toBool();
    m_processing.voice.compressor = settings.value(QLatin1String(keyCompressor), false).toBool();
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
    settings.setValue(QLatin1String(keyVoiceEffect), voiceEffect());
    settings.setValue(QLatin1String(keyEffectIntensity), effectIntensity());
    settings.setValue(QLatin1String(keyStudio), studioVoice());
    settings.setValue(QLatin1String(keyNoiseReduction), noiseReduction());
    settings.setValue(QLatin1String(keyAutomaticGain), automaticGain());
    settings.setValue(QLatin1String(keyCompressor), compressor());
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
    setVoiceEffect(QStringLiteral("none"));
    setEffectIntensity(0.5);
    setStudioVoice(false);
    setNoiseReduction(false);
    setAutomaticGain(false);
    setCompressor(false);
}

QVariantList MicrophoneSettings::voiceEffects() const
{
    QVariantList list;
    for (const auto &effect : effects)
        list.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(effect.id)},
                                {QStringLiteral("name"), QString::fromLatin1(effect.name)}});
    return list;
}

QString MicrophoneSettings::voiceEffect() const
{
    return QString::fromLatin1(effects[int(m_processing.voice.effect)].id);
}

void MicrophoneSettings::processingUpdated()
{
    m_monitor.setConfig(m_processing);
    save();
    emit processingChanged();
}

void MicrophoneSettings::setVoiceEffect(const QString &id)
{
    for (int i = 0; i < int(std::size(effects)); ++i) {
        if (id != QLatin1String(effects[i].id))
            continue;
        if (int(m_processing.voice.effect) == i)
            return;
        m_processing.voice.effect = static_cast<VoiceEffects::Effect>(i);
        processingUpdated();
        return;
    }
}

void MicrophoneSettings::setEffectIntensity(double intensity)
{
    intensity = std::clamp(std::isfinite(intensity) ? intensity : 0.5, 0.0, 1.0);
    if (qFuzzyCompare(m_processing.voice.intensity, intensity)) return;
    m_processing.voice.intensity = intensity;
    processingUpdated();
}

void MicrophoneSettings::setStudioVoice(bool enabled)
{
    if (m_processing.voice.studio == enabled) return;
    m_processing.voice.studio = enabled;
    processingUpdated();
}

void MicrophoneSettings::setNoiseReduction(bool enabled)
{
    if (m_processing.voice.noiseReduction == enabled) return;
    m_processing.voice.noiseReduction = enabled;
    processingUpdated();
}

void MicrophoneSettings::setAutomaticGain(bool enabled)
{
    if (m_processing.voice.automaticGain == enabled) return;
    m_processing.voice.automaticGain = enabled;
    processingUpdated();
}

void MicrophoneSettings::setCompressor(bool enabled)
{
    if (m_processing.voice.compressor == enabled) return;
    m_processing.voice.compressor = enabled;
    processingUpdated();
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
