#pragma once

// Scriptable stand-ins for the microphone and the speaker, shared by the call
// tests. Capture emits whatever the test hands it, on demand; playback records
// what it is asked for. No audio hardware is ever opened, so these run headless
// and deterministically.

#include "call/AudioIo.h"
#include "media/AudioTypes.h"

#include <functional>
#include <memory>

namespace OpenChat::CallTest {

class ScriptedCapture final : public AudioCaptureSource
{
public:
    // The device state the test keeps hold of after the engine has taken
    // ownership of the device object itself.
    struct Shared final {
        bool started = false;
        int stopCount = 0;
        std::function<void(const AudioFrame &)> sink;
        bool failToStart = false;
    };

    explicit ScriptedCapture(std::shared_ptr<Shared> shared)
        : m_shared(std::move(shared))
    {
    }

    bool start() override
    {
        if (m_shared->failToStart)
            return false;
        m_shared->started = true;
        m_shared->sink = onFrame;
        return true;
    }

    void stop() override
    {
        m_shared->started = false;
        ++m_shared->stopCount;
        m_shared->sink = nullptr;
    }

private:
    std::shared_ptr<Shared> m_shared;
};

class ScriptedPlayback final : public AudioPlaybackSink
{
public:
    struct Shared final {
        bool started = false;
        std::function<AudioFrame()> source;
    };

    explicit ScriptedPlayback(std::shared_ptr<Shared> shared)
        : m_shared(std::move(shared))
    {
    }

    bool start() override
    {
        m_shared->started = true;
        m_shared->source = pullFrame;
        return true;
    }

    void stop() override
    {
        m_shared->started = false;
        m_shared->source = nullptr;
    }

private:
    std::shared_ptr<Shared> m_shared;
};

// A microphone and a speaker whose state the test can still reach.
struct ScriptedAudioDevices final {
    std::shared_ptr<ScriptedCapture::Shared> capture =
        std::make_shared<ScriptedCapture::Shared>();
    std::shared_ptr<ScriptedPlayback::Shared> playback =
        std::make_shared<ScriptedPlayback::Shared>();

    [[nodiscard]] CallAudioIoFactory factory() const
    {
        CallAudioIoFactory made;
        made.makeCapture = [capture = capture] {
            return std::make_unique<ScriptedCapture>(capture);
        };
        made.makePlayback = [playback = playback] {
            return std::make_unique<ScriptedPlayback>(playback);
        };
        return made;
    }

    // Pushes one captured frame through the call, as a real device would.
    void speak(const AudioFrame &frame) const
    {
        if (capture->sink)
            capture->sink(frame);
    }

    // Takes the frame the speaker would render now.
    [[nodiscard]] AudioFrame listen() const
    {
        return playback->source ? playback->source() : AudioFrame();
    }
};

} // namespace OpenChat::CallTest
