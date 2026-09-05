#pragma once

#include "media/AudioTypes.h"

#include <functional>
#include <memory>

namespace OpenChat {

// The microphone, reduced to the one thing a call needs from it: a 20 ms frame,
// in the call format, delivered on a steady cadence.
//
// Capture pushes because that is how every real audio API delivers it — the
// device decides when a buffer is ready.
class AudioCaptureSource
{
public:
    virtual ~AudioCaptureSource() = default;

    [[nodiscard]] virtual bool start() = 0;
    virtual void stop() = 0;

    // Installed by the call before start(). Called once per captured frame.
    std::function<void(const AudioFrame &)> onFrame;
};

// The speaker, as a pull: the device asks for the next frame when it is about to
// run out.
//
// Pull rather than push is what keeps a call in sync. The playback clock is the
// only steady clock in the system, so letting it drive the jitter buffer means
// exactly one frame is dequeued per frame interval — no drift, no queue growth,
// and loss concealment lands in the right slot.
class AudioPlaybackSink
{
public:
    virtual ~AudioPlaybackSink() = default;

    [[nodiscard]] virtual bool start() = 0;
    virtual void stop() = 0;

    // Installed by the call before start(). Must return exactly one full frame
    // every time it is called; the call's session guarantees that.
    std::function<AudioFrame()> pullFrame;
};

// How a call obtains its devices. Injecting this is what lets a call run with
// a file for a microphone and a buffer for a speaker.
struct CallAudioIoFactory final {
    std::function<std::unique_ptr<AudioCaptureSource>()> makeCapture;
    std::function<std::unique_ptr<AudioPlaybackSink>()> makePlayback;

    [[nodiscard]] bool isValid() const noexcept
    {
        return static_cast<bool>(makeCapture) && static_cast<bool>(makePlayback);
    }
};

} // namespace OpenChat
