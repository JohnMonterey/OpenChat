#pragma once

#include "call/AudioIo.h"

#include <QAudioFormat>
#include <QIODevice>
#include <QObject>

#include <memory>

QT_BEGIN_NAMESPACE
class QAudioSink;
class QAudioSource;
QT_END_NAMESPACE

namespace OpenChat {

// The QAudioFormat matching CallAudioFormat exactly. Kept in one place so the
// capture and playback devices cannot be opened with formats that disagree.
[[nodiscard]] QAudioFormat callQAudioFormat();

// True when this machine has both a usable default input and a usable default
// output for the call format. Checked before a call is offered so "you have no
// microphone" is reported up front rather than as a call that fails to connect.
[[nodiscard]] bool hasUsableCallAudioDevices();

// Microphone capture through QAudioSource in pull mode: Qt writes into the
// QIODevice below, which slices the stream into exact 20 ms frames. Slicing here
// rather than trusting the device's buffer size is what keeps the sequence
// numbering aligned with real time on every backend.
class QtAudioCaptureSource final : public QObject, public AudioCaptureSource
{
    Q_OBJECT

public:
    explicit QtAudioCaptureSource(QObject *parent = nullptr);
    ~QtAudioCaptureSource() override;

    [[nodiscard]] bool start() override;
    void stop() override;

private:
    class FrameSlicer;

    std::unique_ptr<QAudioSource> m_source;
    std::unique_ptr<FrameSlicer> m_slicer;
};

// Speaker playback through QAudioSink in pull mode: Qt reads from the QIODevice
// below, which asks the call for the next frame whenever it needs one. That
// makes the sound card's clock the call's clock, which is what keeps the jitter
// buffer draining at exactly the rate audio is consumed.
class QtAudioPlaybackSink final : public QObject, public AudioPlaybackSink
{
    Q_OBJECT

public:
    explicit QtAudioPlaybackSink(QObject *parent = nullptr);
    ~QtAudioPlaybackSink() override;

    [[nodiscard]] bool start() override;
    void stop() override;

private:
    class FramePump;

    std::unique_ptr<QAudioSink> m_sink;
    std::unique_ptr<FramePump> m_pump;
};

// The factory the app installs on the call engine: real devices, opened lazily
// when a call actually starts.
[[nodiscard]] CallAudioIoFactory makeQtCallAudioIoFactory();

} // namespace OpenChat
