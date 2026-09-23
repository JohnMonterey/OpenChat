#include "CaseAudio.h"
#include "CaseMotion.h"
#include "media/WavFile.h"
#include <QAudioDevice>
#include <QMediaDevices>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace OpenChat {
void CaseSampleStream::trigger(const QByteArray &sample)
{
    QMutexLocker lock(&m_mutex);
    if (m_crossfade > 0 && m_cursor < m_sample.size()) {
        // What remains of the outgoing voice fades across the crossfade
        // window; a longer remnant than that is not worth preserving.
        m_tail = m_sample.mid(int(m_cursor));
        if (m_tail.size() > m_crossfade) m_tail.resize(int(m_crossfade));
    } else {
        m_tail.clear();
    }
    m_tailCursor = 0;
    m_sample = sample;
    m_cursor = 0;
}
qint64 CaseSampleStream::readData(char *data, qint64 maximum)
{
    QMutexLocker lock(&m_mutex);
    qint64 offset = 0;
    // The fading tail mixes (with saturation, never wrap) into whatever the
    // incoming voice sounds in the window, then the new voice is a plain copy.
    while (offset + 1 < maximum && m_tailCursor < m_tail.size()) {
        const auto tail = qFromLittleEndian<qint16>(m_tail.constData() + m_tailCursor);
        const double fade = 1.0 - double(m_tailCursor) / m_tail.size();
        int value = int(tail * fade);
        if (m_cursor + 1 < m_sample.size()) {
            value += qFromLittleEndian<qint16>(m_sample.constData() + m_cursor);
            m_cursor += 2;
        }
        qToLittleEndian(qint16(std::clamp(value, -32768, 32767)), data + offset);
        m_tailCursor += 2;
        offset += 2;
    }
    if (offset < maximum) {
        const auto count = std::min(maximum - offset, m_sample.size() - m_cursor);
        if (count > 0) std::memcpy(data + offset, m_sample.constData() + m_cursor, size_t(count));
        std::memset(data + offset + count, 0, size_t(maximum - offset - count));
        m_cursor += count;
    }
    return maximum;
}
namespace {
// Counter-Strike's crate UI sounds, bundled by the QML module's resources.
constexpr auto displayPath = QLatin1String(":/qt/qml/OpenChat/assets/sounds/crate-display.wav");
constexpr auto scrollPath = QLatin1String(":/qt/qml/OpenChat/assets/sounds/crate-item-scroll.wav");
constexpr auto openPath = QLatin1String(":/qt/qml/OpenChat/assets/sounds/crate-open.wav");
// Like the call interface sounds, these sit below full scale: the case may
// open while a call is running, and a UI flourish must not shout over speech.
constexpr double crateGain = 0.5;

// Decodes a bundled WAV and converts it to the sink's S16 layout: linear
// interpolation between rates, stereo folded for mono sinks, extra sink
// channels carried by the left channel.
QByteArray sampleForSink(const QString &path, const QAudioFormat &format)
{
    const auto wav = WavFile::readFile(path);
    if (!wav) return {};
    const auto &audio = wav.value();
    if (audio.frameCount() <= 0) return {};
    const int channels = std::max(1, format.channelCount());
    const int rate = std::max(1, format.sampleRate());
    const int sourceChannels = std::max(1, audio.channels);
    const double sourceRate = std::max(1, audio.sampleRate);
    const auto &source = audio.samples;
    const qsizetype frames = audio.frameCount();
    const qsizetype outFrames = qsizetype(std::ceil(frames * rate / sourceRate)) + 1;
    QByteArray pcm(outFrames * channels * int(sizeof(qint16)), Qt::Uninitialized);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    for (qsizetype i = 0; i < outFrames; ++i) {
        const double position = std::min(double(frames - 1), i * sourceRate / rate);
        const qsizetype frame0 = qsizetype(position);
        const qsizetype frame1 = std::min(frames - 1, frame0 + 1);
        const double blend = position - frame0;
        for (int c = 0; c < channels; ++c) {
            double a, b;
            if (channels == 1 && sourceChannels == 2) {
                a = (source[frame0 * 2] + source[frame0 * 2 + 1]) / 2.0;
                b = (source[frame1 * 2] + source[frame1 * 2 + 1]) / 2.0;
            } else {
                const int channel = std::min(c, sourceChannels - 1);
                a = source[frame0 * sourceChannels + channel];
                b = source[frame1 * sourceChannels + channel];
            }
            *out++ = qint16(std::clamp((a + (b - a) * blend) * crateGain, -32768.0, 32767.0));
        }
    }
    return pcm;
}
}
void CaseAudio::start()
{
    stop();
    const auto device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) return;
    auto format = device.preferredFormat();
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) return;
    m_display = sampleForSink(displayPath, format);
    m_tick = sampleForSink(scrollPath, format);
    m_open = sampleForSink(openPath, format);
    if (m_tick.isEmpty() && m_open.isEmpty() && m_display.isEmpty()) return;
    m_sink = std::make_unique<QAudioSink>(device, format);
    m_sink->setBufferSize(format.bytesForDuration(16000));
    m_stream.setCrossfadeBytes(std::max<qint64>(2, format.bytesForDuration(3000)));
    m_sink->start(&m_stream);
}
int CaseAudio::display()
{
    if (!m_sink || m_display.isEmpty()) return 0;
    m_stream.trigger(m_display);
    const auto &format = m_sink->format();
    return int(qint64(m_display.size()) * 1000
               / (format.bytesPerFrame() * format.sampleRate()));
}
void CaseAudio::tick()
{
    if (m_lastTick.isValid() && m_lastTick.elapsed() < CaseMotion::minimumTickMs) return;
    m_lastTick.start();
    if (m_sink && !m_tick.isEmpty()) m_stream.trigger(m_tick);
}
int CaseAudio::impact()
{
    if (!m_sink || m_open.isEmpty()) return 0;
    m_stream.trigger(m_open);
    const auto &format = m_sink->format();
    return int(qint64(m_open.size()) * 1000 / (format.bytesPerFrame() * format.sampleRate()));
}
void CaseAudio::stop()
{
    if (m_sink) m_sink->stop();
    m_sink.reset();
    m_stream.trigger({});
    m_lastTick.invalidate();
}
}
