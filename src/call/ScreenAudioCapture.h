#pragma once

#include "call/ScreenAudio.h"

#include <QMutex>
#include <QString>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace OpenChat {

// Capturing what a computer is playing, for a screen share's sound.
//
// The one rule every platform keeps: OpenChat's own output is never captured.
// That output is the other people in the call; capturing it would send each of
// them their own voice back, a fraction of a second late.
//
//   Windows  WASAPI process loopback (Windows 10 2004 and later): everything
//            except OpenChat's process tree for a screen; for a window, that
//            window's application alone.
//   Linux    PulseAudio's API (PipeWire serves it too): one monitor stream per
//            application playing, skipping OpenChat's, summed.
//   macOS    not yet.

// What to capture the sound of.
struct ScreenAudioTarget final {
    // A whole screen: every application except OpenChat. A window: that
    // window's application alone, where the platform can tell which one it is
    // (Windows); elsewhere the same as a screen.
    bool window = false;
    // The platform's handle for the window (an HWND on Windows), or 0.
    quint64 nativeWindow = 0;
};

class ScreenAudioCapture
{
public:
    virtual ~ScreenAudioCapture() = default;

    // Starts capturing. On failure returns false with a sentence a person can
    // act on in `failure`.
    [[nodiscard]] virtual bool start(QString &failure) = 0;
    // Stops, and returns only once `onFrame` can no longer be called.
    virtual void stop() = 0;
    // What is being captured and through which API, for logs and reports.
    // Never names an application or a window title.
    [[nodiscard]] virtual QString describe() const = 0;

    // One 20 ms stereo frame, every 20 ms, silence included, on the capture's
    // own thread. Installed before start().
    std::function<void(const StereoFrame &)> onFrame;
};

namespace ScreenAudioCapturePlatform {

// Whether this build on this system can share sound at all.
[[nodiscard]] bool isSupported();
// Why not, when it cannot; empty otherwise.
[[nodiscard]] QString unsupportedReason();
[[nodiscard]] std::unique_ptr<ScreenAudioCapture> create(const ScreenAudioTarget &target);
// The API in use, for logs ("WASAPI process loopback").
[[nodiscard]] QString backendName();

} // namespace ScreenAudioCapturePlatform

// Turns audio that arrives in whatever pieces a platform hands over, from one
// source or several, into one steady 20 ms stereo frame every 20 ms.
//
// The cadence is the clock's, not the sources': a stream of frames with no
// gaps is what the far end's jitter buffer is built for, and a source that
// goes quiet (nothing playing) simply contributes silence. Each source keeps a
// small cushion so a piece arriving a moment late does not leave a hole in
// the frame it belongs to, and a source that runs ahead of the clock (every
// sound card's crystal is a little off) is trimmed back rather than allowed to
// build up delay.
//
// Thread-safe: sources push from their own threads, one thread pumps.
class ScreenAudioFramer final
{
public:
    // Frames a source holds before it starts contributing: 40 ms.
    static constexpr int cushionFrames = 2;
    // More than this and the oldest is dropped back to the cushion: 120 ms.
    static constexpr int ceilingFrames = 6;

    // Interleaved 48 kHz stereo S16 samples; `frames` counts sample pairs.
    void push(int source, const qint16 *interleaved, qsizetype frames);
    void pushSilence(int source, qsizetype frames);
    void removeSource(int source);

    // Emits every frame due by `nowMs`. The first call starts the clock.
    void pump(qint64 nowMs, const std::function<void(const StereoFrame &)> &deliver);

    // Frames dropped to keep up and frames emitted, for diagnostics.
    [[nodiscard]] quint64 trimmedFrames() const;
    [[nodiscard]] quint64 emittedFrames() const;

private:
    struct Source final {
        std::deque<qint16> samples; // interleaved
        bool primed = false;
    };

    mutable QMutex m_mutex;
    std::map<int, Source> m_sources;
    qint64 m_nextDueMs = -1;
    quint64 m_trimmed = 0;
    quint64 m_emitted = 0;
};

// Converts one block of device audio to 48 kHz interleaved stereo S16: any
// sample rate (linear interpolation, carried across blocks), float or 16-bit,
// any channel count (mono is spread to both sides; beyond two, the first two
// are kept). For the capture paths that cannot ask the system for our format.
class StereoConverter final
{
public:
    enum class Sample { Int16, Float32 };

    StereoConverter(int sampleRate, int channels, Sample sample);

    [[nodiscard]] bool isValid() const noexcept { return m_rate > 0 && m_channels > 0; }
    // Appends the converted samples to `out`.
    void convert(const void *data, qsizetype frames, std::vector<qint16> &out);

private:
    int m_rate = 0;
    int m_channels = 0;
    Sample m_sample = Sample::Int16;
    // Resampling position, in input frames, and the last input frame, so the
    // interpolation runs on across blocks.
    double m_position = 0.0;
    float m_last[2] = {0.0F, 0.0F};
    bool m_haveLast = false;
};

} // namespace OpenChat
