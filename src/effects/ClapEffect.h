#pragma once

#include "effects/AudioPluginInstance.h"
#include "media/AudioTypes.h"

#include <clap/clap.h>

#include <QHash>

#include <memory>
#include <vector>

namespace OpenChat {

class ClapModule;

// The fixed-capacity parameter-event queue a plugin is handed on every frame.
//
// clap_process_t::in_events and out_events must NEVER be null. This is not
// defensive style: some real plugins null-check them and survive, and others
// dereference them and take the process down, so a host that passes null works
// perfectly until the day somebody loads the other kind. An empty, well-formed
// list is what "no events this frame" looks like.
//
// Capacity is fixed and events past it are dropped, because the alternative is
// allocating inside a frame. Sixty-four parameter changes in one 20 ms block is
// already far more than a human can produce.
class ClapEventQueue final
{
public:
    ClapEventQueue();

    ClapEventQueue(const ClapEventQueue &) = delete;
    ClapEventQueue &operator=(const ClapEventQueue &) = delete;

    // Queued for the head of the next frame. Silently dropped when full, which
    // is the right failure: losing one knob movement out of sixty-four is
    // invisible, and allocating here would not be.
    void pushParameterValue(clap_id parameterId, void *cookie, double value);

    void clear() noexcept { m_events.clear(); }

    [[nodiscard]] const clap_input_events_t *input() const noexcept { return &m_input; }
    [[nodiscard]] const clap_output_events_t *output() const noexcept { return &m_output; }

private:
    static uint32_t inputSize(const clap_input_events_t *list);
    static const clap_event_header_t *inputGet(const clap_input_events_t *list, uint32_t index);
    static bool outputTryPush(const clap_output_events_t *list, const clap_event_header_t *event);

    static constexpr int capacity = 64;

    std::vector<clap_event_param_value_t> m_events;
    clap_input_events_t m_input{};
    clap_output_events_t m_output{};
};

// One activated CLAP plugin, adapted to a mono 48 kHz 20 ms voice frame.
//
// The adaptation is the interesting part, because CLAP has no bus negotiation
// at all: clap_plugin_audio_ports has count() and get() and nothing that sets,
// so the layout is take-it-or-leave-it and the HOST is what bends. Three rules,
// each learned from a plugin that punishes getting it wrong:
//
//  - Allocate a real, zeroed buffer for every channel of every DECLARED port,
//    sidechains included. clap/process.h requires audio_inputs_count to equal
//    the declared count, and sidechain-gate plugins dereference the second port
//    without checking. A host that assumes one port segfaults on them.
//  - Find the main port by testing the IS_MAIN BIT, never by comparing the
//    flags word. Different plugin frameworks publish different flag
//    combinations -- 0x9/0x8 in one, 0x1/0x0 in another -- and bit 0 is the
//    only thing they agree on.
//  - Take channel 0 of the main output, never the average of the channels. A
//    stereo widener, a Haas processor or a ping-pong delay puts partially
//    anti-phase content in its two channels, and averaging those cancels
//    exactly the thing the effect added: near-silence out of a plugin that is
//    working perfectly.
class ClapEffect final : public AudioPluginInstance
{
public:
    ~ClapEffect() override;

    ClapEffect(const ClapEffect &) = delete;
    ClapEffect &operator=(const ClapEffect &) = delete;

    // Takes a shared reference to the module so the .so outlives the instance.
    [[nodiscard]] static std::unique_ptr<ClapEffect> create(std::shared_ptr<ClapModule> module,
                                                           const clap_plugin_t *plugin,
                                                           AudioPluginDescriptor descriptor,
                                                           PluginError &error);

    void processMono(float *samples) noexcept override;
    void reset() noexcept override;

    [[nodiscard]] const AudioPluginDescriptor &descriptor() const noexcept override
    {
        return m_descriptor;
    }
    [[nodiscard]] int latencySamples() const noexcept override { return m_latencySamples; }

    void setParameter(quint32 parameterId, double value) override;
    [[nodiscard]] double parameterValue(quint32 parameterId) const override;
    [[nodiscard]] QString parameterText(quint32 parameterId, double value) const override;

    [[nodiscard]] QByteArray saveState() const override;
    bool loadState(const QByteArray &state) override;

private:
    ClapEffect() = default;

    // Reads the port layout and allocates every buffer the plugin will be
    // handed. Everything that could allocate happens here, so that a frame
    // never does.
    [[nodiscard]] PluginError configurePorts();
    [[nodiscard]] PluginError activate();
    void deactivate() noexcept;

    std::shared_ptr<ClapModule> m_module;
    const clap_plugin_t *m_plugin = nullptr;
    AudioPluginDescriptor m_descriptor;

    const clap_plugin_audio_ports_t *m_ports = nullptr;
    const clap_plugin_params_t *m_params = nullptr;
    const clap_plugin_state_t *m_state = nullptr;
    const clap_plugin_latency_t *m_latency = nullptr;

    // Per-port channel storage. One flat vector per port holding
    // channelCount * samplesPerFrame floats, plus the array of per-channel
    // pointers into it that the ABI actually wants.
    std::vector<std::vector<float>> m_inStorage;
    std::vector<std::vector<float>> m_outStorage;
    std::vector<std::vector<float *>> m_inPtrs;
    std::vector<std::vector<float *>> m_outPtrs;
    std::vector<clap_audio_buffer_t> m_inBuffers;
    std::vector<clap_audio_buffer_t> m_outBuffers;
    int m_mainIn = 0;
    int m_mainOut = 0;

    ClapEventQueue m_events;
    QHash<quint32, void *> m_parameterCookies;
    quint64 m_steadyTime = 0;
    int m_latencySamples = 0;
    bool m_active = false;
    bool m_processing = false;
};

} // namespace OpenChat
