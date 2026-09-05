#include "call/QtAudioIo.h"

#include "media/AudioConvert.h"
#include "media/AudioTypes.h"

#include <QAudioSink>
#include <QAudioSource>
#include <QMediaDevices>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace OpenChat {

QAudioFormat callQAudioFormat()
{
    QAudioFormat format;
    format.setSampleRate(CallAudioFormat::sampleRate);
    format.setChannelCount(CallAudioFormat::channels);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}

QAudioFormat callPlaybackQAudioFormat()
{
    QAudioFormat format = callQAudioFormat();
    format.setChannelCount(playbackChannels);
    return format;
}

namespace {

QAudioFormat captureFormat(const QAudioDevice &device)
{
    // Preserve the interface's channels so the driver cannot silently select
    // just input 1 when the microphone is plugged into input 2.
    QAudioFormat format = device.preferredFormat();
    format.setSampleRate(CallAudioFormat::sampleRate);
    if (device.isFormatSupported(format))
        return format;
    return device.preferredFormat();
}

} // namespace

AudioFrame mixCaptureFrame(const QByteArray &pcm, const QAudioFormat &format)
{
    if (!format.isValid() || pcm.size() != format.bytesForDuration(20'000))
        return {};
    QVector<qint16> mono;
    const int channels = format.channelCount();
    const int stride = format.bytesPerSample();
    mono.reserve(pcm.size() / format.bytesPerFrame());
    for (qsizetype offset = 0; offset < pcm.size(); offset += format.bytesPerFrame()) {
        double sum = 0;
        for (int channel = 0; channel < channels; ++channel) {
            const double sample =
                format.normalizedSampleValue(pcm.constData() + offset + channel * stride);
            sum += std::isfinite(sample) ? sample : 0.0;
        }
        mono.append(static_cast<qint16>(
            std::clamp(std::round(sum * 32768.0 / channels), -32768.0, 32767.0)));
    }
    return AudioConvert::frameOf(AudioConvert::resampleMono(
        mono, format.sampleRate(), CallAudioFormat::sampleRate));
}

bool hasUsableCallAudioDevices()
{
    const QAudioDevice input = QMediaDevices::defaultAudioInput();
    const QAudioDevice output = QMediaDevices::defaultAudioOutput();
    return !input.isNull() && !output.isNull() && input.isFormatSupported(captureFormat(input))
        && output.isFormatSupported(callPlaybackQAudioFormat());
}

// Accumulates whatever byte counts the audio backend hands over and emits
// exactly one full frame at a time. Backends deliver wildly different chunk
// sizes (and partial ones), so a call cannot assume its frames arrive
// pre-aligned.
class QtAudioCaptureSource::FrameSlicer final : public QIODevice
{
public:
    explicit FrameSlicer(QtAudioCaptureSource &owner, QAudioFormat format)
        : m_owner(owner), m_format(format)
    {
    }

protected:
    qint64 readData(char *, qint64) override { return -1; } // write-only sink

    qint64 writeData(const char *data, qint64 length) override
    {
        if (length <= 0)
            return 0;
        m_pending.append(data, static_cast<qsizetype>(length));
        const int captureBytes = m_format.bytesForDuration(20'000);
        while (m_pending.size() >= captureBytes) {
            const AudioFrame frame = mixCaptureFrame(m_pending.left(captureBytes), m_format);
            m_pending.remove(0, captureBytes);
            // Hop to the owner's thread before handing the frame on. Some Qt
            // audio backends deliver from their own thread, and the call's
            // reaction to a captured frame is to sign it and write it to a
            // WebSocket — neither of which is safe off the owning thread. The
            // queued hop costs one event-loop turn against a 20 ms budget.
            QMetaObject::invokeMethod(
                &m_owner,
                [owner = &m_owner, frame] {
                    if (owner->onFrame)
                        owner->onFrame(frame);
                },
                Qt::QueuedConnection);
        }
        return length;
    }

private:
    QtAudioCaptureSource &m_owner;
    QAudioFormat m_format;
    QByteArray m_pending;
};

QtAudioCaptureSource::QtAudioCaptureSource(QObject *parent)
    : QObject(parent)
{
}

QtAudioCaptureSource::~QtAudioCaptureSource()
{
    stop();
}

bool QtAudioCaptureSource::start()
{
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull())
        return false;
    const auto format = captureFormat(device);
    if (!format.isValid() || !device.isFormatSupported(format))
        return false;
    m_source = std::make_unique<QAudioSource>(device, format);
    m_slicer = std::make_unique<FrameSlicer>(*this, format);
    if (!m_slicer->open(QIODevice::WriteOnly))
        return false;
    m_source->start(m_slicer.get());
    return m_source->error() == QAudio::NoError;
}

void QtAudioCaptureSource::stop()
{
    if (m_source) {
        m_source->stop();
        m_source.reset();
    }
    if (m_slicer) {
        m_slicer->close();
        m_slicer.reset();
    }
}

// Supplies the sink with audio on demand, one frame at a time, padding whatever
// the backend asks for out of the call. Returning short would let the sink
// underrun; asking the call for more than one frame per read would drain the
// jitter buffer faster than real time.
//
// The call hands over mono frames; the sink is stereo (see
// callPlaybackQAudioFormat), so each sample is written to both channels here.
//
// Unlike capture this CANNOT hop threads: the backend wants the bytes now. The
// call session it pulls from is internally synchronised for exactly this reason.
class QtAudioPlaybackSink::FramePump final : public QIODevice
{
public:
    explicit FramePump(QtAudioPlaybackSink &owner)
        : m_owner(owner)
    {
    }

protected:
    qint64 writeData(const char *, qint64) override { return -1; } // read-only source

    qint64 readData(char *data, qint64 maxLength) override
    {
        qint64 written = 0;
        while (written < maxLength) {
            if (m_pending.isEmpty()) {
                if (!m_owner.pullFrame)
                    break;
                m_pending = upmixToStereo(m_owner.pullFrame());
                if (m_pending.isEmpty())
                    break;
            }
            const qint64 chunk = std::min<qint64>(maxLength - written, m_pending.size());
            std::memcpy(data + written, m_pending.constData(), static_cast<size_t>(chunk));
            m_pending.remove(0, static_cast<qsizetype>(chunk));
            written += chunk;
        }
        // A sink that is handed 0 bytes stops asking on some backends, so fill
        // the remainder with silence rather than returning short.
        if (written < maxLength) {
            std::memset(data + written, 0, static_cast<size_t>(maxLength - written));
            written = maxLength;
        }
        return written;
    }

    // Pull-mode sinks poll this to decide whether to read; a call always has
    // something to play (silence at worst), so it is never empty.
    [[nodiscard]] qint64 bytesAvailable() const override
    {
        return playbackBytesPerFrame + QIODevice::bytesAvailable();
    }

private:
    // Interleaves a mono S16LE frame as L R L R ..., the same sample in both.
    [[nodiscard]] static QByteArray upmixToStereo(const AudioFrame &mono)
    {
        constexpr int bytesPerSample = CallAudioFormat::bytesPerSample;
        const qsizetype samples = mono.size() / bytesPerSample;
        QByteArray stereo(samples * bytesPerSample * playbackChannels, Qt::Uninitialized);
        const char *in = mono.constData();
        char *out = stereo.data();
        for (qsizetype i = 0; i < samples; ++i) {
            std::memcpy(out, in, bytesPerSample);
            std::memcpy(out + bytesPerSample, in, bytesPerSample);
            in += bytesPerSample;
            out += bytesPerSample * playbackChannels;
        }
        return stereo;
    }

    QtAudioPlaybackSink &m_owner;
    QByteArray m_pending;
};

QtAudioPlaybackSink::QtAudioPlaybackSink(QObject *parent)
    : QObject(parent)
{
}

QtAudioPlaybackSink::~QtAudioPlaybackSink()
{
    stop();
}

bool QtAudioPlaybackSink::start()
{
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull())
        return false;
    m_sink = std::make_unique<QAudioSink>(device, callPlaybackQAudioFormat());
    m_pump = std::make_unique<FramePump>(*this);
    if (!m_pump->open(QIODevice::ReadOnly))
        return false;
    m_sink->start(m_pump.get());
    return m_sink->error() == QAudio::NoError;
}

void QtAudioPlaybackSink::stop()
{
    if (m_sink) {
        m_sink->stop();
        m_sink.reset();
    }
    if (m_pump) {
        m_pump->close();
        m_pump.reset();
    }
}

CallAudioIoFactory makeQtCallAudioIoFactory()
{
    CallAudioIoFactory factory;
    factory.makeCapture = [] { return std::make_unique<QtAudioCaptureSource>(); };
    factory.makePlayback = [] { return std::make_unique<QtAudioPlaybackSink>(); };
    return factory;
}

} // namespace OpenChat
