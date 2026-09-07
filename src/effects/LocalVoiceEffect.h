#pragma once

#include "effects/AudioPluginTrust.h"
#include "effects/AudioPluginTypes.h"
#include "effects/VoiceEffect.h"
#include "effects/VoiceEffectChain.h"

#include <QList>

namespace OpenChat {

// The VoiceEffect a call actually gets: a chain of plugins, running in this
// process.
//
// IN THIS PROCESS is the whole caveat, and it is not a small one. See the
// SECURITY section of VoiceEffect.h: a plugin loaded here can read the MLS
// group keys, the SQLCipher key and the device identity key, and can reach the
// network to send them somewhere. The trust layer decides WHICH file gets that
// access; it cannot constrain what the file does once it has it.
//
// This is therefore the implementation to replace, not the one to build on. The
// VoiceEffect interface exists so that a sandboxed helper process -- no session
// bus, no network, no view of the profile directory, frames over a socket -- is
// a sibling class rather than a redesign. Until that exists, the honest summary
// is: the feature is off by default, every plugin is individually approved by
// the user, and a user who approves a hostile plugin has given it everything.
//
// It is also the only place a deadline could be enforced, and today is not:
// captured frames are handled on the GUI thread, so a plugin that blocks for
// three hundred milliseconds freezes the messenger's interface for three
// hundred milliseconds. The chain's overrun counter notices and switches a
// persistently slow plugin off, which bounds the damage over a call but not
// within a frame -- you cannot deadline a function call, only an I/O wait.
class LocalVoiceEffect final : public VoiceEffect
{
public:
    LocalVoiceEffect(VoiceEffectChainConfig config, QList<PluginConsent> consents);
    ~LocalVoiceEffect() override;

    [[nodiscard]] bool prepare() override;
    [[nodiscard]] AudioFrame process(const AudioFrame &frame) override;
    void reset() override;
    [[nodiscard]] VoiceEffectStatus status() const override;

    // What each configured stage did or did not manage to do. Read between
    // frames by a settings panel; never during one.
    [[nodiscard]] const QList<VoiceEffectChain::StageStatus> &stages() const noexcept
    {
        return m_chain.stages();
    }

    void setParameter(int stageIndex, quint32 parameterId, double value)
    {
        m_chain.setParameter(stageIndex, parameterId, value);
    }
    [[nodiscard]] QList<QByteArray> captureState() const { return m_chain.captureState(); }

    // Builds the factory a CallEngine is configured with. Copies the config and
    // consents so that each call gets its own chain: a plugin that failed in one
    // call does not carry its failure into the next, and a chain edited while a
    // call is running does not mutate under it.
    [[nodiscard]] static VoiceEffectFactory factoryFor(VoiceEffectChainConfig config,
                                                       QList<PluginConsent> consents);

private:
    VoiceEffectChainConfig m_config;
    QList<PluginConsent> m_consents;
    VoiceEffectChain m_chain;
    // One reusable output frame. Allocated at prepare() so that process() does
    // not allocate, which matters because it runs on the thread drawing the UI.
    AudioFrame m_out;
    bool m_prepared = false;
};

} // namespace OpenChat
