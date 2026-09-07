#pragma once

#include "effects/AudioPluginInstance.h"
#include "effects/Vst3Support.h"
#include "media/AudioTypes.h"

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <QHash>

#include <memory>
#include <vector>

namespace OpenChat {

class Vst3Module;

// One activated VST3 plugin, adapted to a mono 48 kHz 20 ms voice frame.
//
// Unlike CLAP, VST3 does negotiate: the host proposes a bus arrangement and the
// plugin accepts, adapts or refuses. The sequence matters and most of it is
// only legal while the component is INACTIVE:
//
//     setBusArrangements(kMono, kMono)   <- a proposal, not a command
//     re-read with getBusArrangement     <- the actual answer
//     canProcessSampleSize(kSample32)
//     setupProcessing(...)
//     activateBus(each)
//     setActive(true)
//     setProcessing(true)
//
// Three details in that sequence are where hosts go wrong:
//
//  - kMono is (1 << 19), not 1. A single set bit at position 0 is kSpeakerL --
//    a one-channel LEFT bus -- and proposing it is the most common way a host
//    is told that a perfectly mono-capable plugin cannot do mono.
//  - The return of setBusArrangements is a HINT. kResultTrue is zero in VST3
//    (success is 0, not 1), kResultFalse means "I adapted to something else"
//    rather than "error", and some plugins return values documented nowhere.
//    The only reliable answer is to ask again with getBusArrangement.
//  - Object identity is equality of queryInterface(FUnknown) results, never of
//    interface pointers. A single-component effect's IComponent and
//    IEditController pointers legitimately differ while being one object, and
//    comparing the raw pointers makes a host call terminate() twice on it.
class Vst3Effect final : public AudioPluginInstance
{
public:
    ~Vst3Effect() override;

    Vst3Effect(const Vst3Effect &) = delete;
    Vst3Effect &operator=(const Vst3Effect &) = delete;

    [[nodiscard]] static std::unique_ptr<Vst3Effect> create(std::shared_ptr<Vst3Module> module,
                                                            const AudioPluginDescriptor &descriptor,
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

    // VST3 parameters are normalised to [0, 1] on the wire and plain in the UI.
    // The conversion lives here so that nothing above this class ever has to
    // know the two ABIs disagree about it.
    [[nodiscard]] static double toNormalised(const AudioPluginParameter &parameter, double plain);
    [[nodiscard]] static double toPlain(const AudioPluginParameter &parameter, double normalised);

private:
    Vst3Effect() = default;

    [[nodiscard]] PluginError negotiateBuses();
    [[nodiscard]] PluginError activate();
    void teardown() noexcept;

    std::shared_ptr<Vst3Module> m_module;
    AudioPluginDescriptor m_descriptor;

    Steinberg::Vst::IComponent *m_component = nullptr;
    Steinberg::Vst::IAudioProcessor *m_processor = nullptr;
    Steinberg::Vst::IEditController *m_controller = nullptr;
    // True when component and controller are the same object, in which case the
    // controller must NOT be terminated or released a second time.
    bool m_singleComponent = false;

    Vst3ComponentHandler m_handler;
    Vst3ParamChanges m_inputChanges;
    Vst3ParamChanges m_outputChanges;

    std::vector<std::vector<float>> m_inStorage;
    std::vector<std::vector<float>> m_outStorage;
    std::vector<std::vector<float *>> m_inPtrs;
    std::vector<std::vector<float *>> m_outPtrs;
    std::vector<Steinberg::Vst::AudioBusBuffers> m_inBuffers;
    std::vector<Steinberg::Vst::AudioBusBuffers> m_outBuffers;

    QHash<quint32, AudioPluginParameter> m_parametersById;
    int m_latencySamples = 0;
    bool m_active = false;
    bool m_processing = false;
};

} // namespace OpenChat
