#pragma once

#include "effects/AudioPluginInstance.h"
#include "effects/AudioPluginTrust.h"
#include "effects/AudioPluginTypes.h"
#include "media/AudioTypes.h"

#include <QList>
#include <QString>

#include <memory>
#include <vector>

namespace OpenChat {

// N plugins in series over one scratch buffer.
//
// Series and not parallel because a chain is what a voice goes through, and
// every effect anybody asks for on a call -- gate, then pitch, then a little
// reverb -- is a sequence. Parallel routing needs a mixer, which is precisely
// what this is not.
//
// The chain is also where a misbehaving plugin is contained as far as it can be
// contained in-process. It cannot be stopped from reading memory, but it can be
// stopped from ruining the call:
//
//  - A stage that produces non-finite samples is switched off, not clamped
//    forever. NaN propagates: once a filter's state is NaN every future sample
//    is too, so clamping would hide a dead plugin behind plausible silence
//    while the far end heard nothing at all.
//  - A stage that consistently costs more than the frame it is processing is
//    switched off, because a plugin slower than real time does not degrade
//    gracefully -- it falls further behind on every frame for the rest of the
//    call.
//  - Switching off crossfades to the dry signal across the frame it happens on,
//    rather than cutting. Jumping from a processed signal to an unprocessed one
//    mid-waveform is a step discontinuity, which is heard as a click -- and a
//    click is what a user reports, rather than the plugin that caused it.
class VoiceEffectChain final
{
public:
    VoiceEffectChain() = default;
    ~VoiceEffectChain();

    VoiceEffectChain(const VoiceEffectChain &) = delete;
    VoiceEffectChain &operator=(const VoiceEffectChain &) = delete;

    // What one stage of the chain reports about itself once the chain is built.
    struct StageStatus final {
        AudioPluginId id;
        QString name;
        bool loaded = false;
        int latencySamples = 0;
        PluginError error;
    };

    // Loads and activates every enabled stage of `config`.
    //
    // `consents` is the user's allowlist and is consulted for every stage: a
    // plugin whose bytes are not approved is skipped with NotApproved rather
    // than loaded. Passing an empty list therefore builds an empty chain, which
    // is the correct default for a caller that has not thought about trust.
    //
    // Returns true when at least one stage loaded. A stage that fails is
    // skipped and reported in stages(); one bad plugin never costs the user the
    // others.
    [[nodiscard]] bool build(const VoiceEffectChainConfig &config,
                             const QList<PluginConsent> &consents);

    void clear() noexcept;

    // One 20 ms frame through every live stage, in order. Never fails: on any
    // trouble the input passes through untouched and the chain reports it.
    void process(const AudioFrame &in, AudioFrame &out) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool isRunning() const noexcept { return m_running && !m_stages.empty(); }
    [[nodiscard]] int activeStageCount() const noexcept
    {
        return static_cast<int>(m_stages.size());
    }
    [[nodiscard]] int latencySamples() const noexcept { return m_latencySamples; }
    [[nodiscard]] quint64 missedFrames() const noexcept { return m_missedFrames; }
    [[nodiscard]] const QList<StageStatus> &stages() const noexcept { return m_stageStatus; }
    // Empty while healthy; otherwise why the chain stopped.
    [[nodiscard]] const QString &error() const noexcept { return m_error; }

    // Applies one parameter to one live stage. Takes effect on the next frame.
    void setParameter(int stageIndex, quint32 parameterId, double value);
    // The plugins' own state blobs, for persisting a chain the user has tuned.
    [[nodiscard]] QList<QByteArray> captureState() const;

private:
    void disableWith(const QString &reason, PluginErrorCode code);

    struct LiveStage final {
        std::shared_ptr<AudioPluginModule> module;
        std::unique_ptr<AudioPluginInstance> instance;
        // Wet/dry for this stage alone, already clamped at build time so the
        // frame path never validates it.
        double mix = 1.0;
    };

    std::vector<LiveStage> m_stages;
    QList<StageStatus> m_stageStatus;

    // One scratch buffer for the whole chain: each stage reads and writes it in
    // place, so N plugins cost one conversion in and one out rather than N.
    std::vector<float> m_scratch;
    // One stage's input, kept only while a stage is partly dry and needs
    // something to blend against. Empty when every stage is fully wet.
    std::vector<float> m_dry;

    int m_latencySamples = 0;
    quint64 m_missedFrames = 0;
    // Consecutive frames that overran. A single slow frame is a scheduling
    // accident; a run of them is a plugin that cannot keep up.
    int m_consecutiveOverruns = 0;
    bool m_running = false;
    QString m_error;
};

} // namespace OpenChat
