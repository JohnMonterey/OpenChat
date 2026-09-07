#pragma once

#include "media/AudioTypes.h"

#include <QString>

#include <functional>
#include <memory>

namespace OpenChat {

// What the effect is doing right now, as a settings panel or a call's
// diagnostics would show it. Read on the owning thread, between frames.
struct VoiceEffectStatus final {
    // False once nothing is loaded, or the chain has switched itself off. The
    // call carries on dry either way; this is what a UI greys out on.
    bool running = false;
    // How many of the configured slots are actually loaded and processing.
    int activeSlots = 0;
    // Summed across active slots, read once at activation. Added straight to
    // mouth-to-ear delay: reported and budgeted, never compensated.
    int latencySamples = 0;
    // Frames that went out untreated. Nonzero means the user was heard dry for
    // a moment; growing means a plugin that cannot keep up.
    quint64 missedFrames = 0;
    // Empty while healthy. Otherwise the one sentence a settings panel shows,
    // already translated and already safe to display.
    QString error;
};

// A third-party effect on the outgoing voice: one 20 ms frame in, one out,
// inserted between the noise gate and the encoder so that every peer in a group
// call hears the same treated audio.
//
// SECURITY -- read this before writing an implementation.
//
// An effect is somebody else's program, and dlopen runs it. This process holds
// the MLS group keys, the SQLCipher key for the entire local message history,
// the device identity key that contacts' safety numbers attest to, and a live
// keychain session that will reissue the profile key on request. Native code
// sharing this address space has all of it without needing an exploit, and it
// has the network, the proxy settings and the CA bundle to send it anywhere.
// Wiping secrets from memory does not help: a plugin can simply ask the Secret
// Service for the profile key the way QtKeychainVault does, because a keyring
// authorises per application and not per module.
//
// Nothing below defends against that, and nothing can while the plugin shares
// this address space. What the subsystem does instead is refuse to get there by
// accident: a plugin is never loaded because it was found, only because the
// user named that exact file and approved its exact bytes. See
// AudioPluginTrust, and docs/voice-effects.md for what is and is not defended.
//
// This is an interface and not a dlopen call for that reason. LocalVoiceEffect
// runs the chain in this process and is what ships today; the boundary exists
// so that an out-of-process implementation -- a sandboxed helper exchanging
// frames over a socket, which is the only way this becomes properly safe -- is
// a new .cpp behind this same seam rather than a redesign. It is also the only
// place a deadline could ever be enforced: in this process a plugin that blocks
// for three hundred milliseconds blocks for three hundred milliseconds and
// takes the messenger's whole interface with it, because captured frames are
// handled on the GUI thread. You cannot deadline a function call, only an I/O
// wait.
//
// Everything here is called on the thread that captured the frame. process() is
// the only per-frame method, and it never fails: a call is never worse off for
// having an effect configured than for not having one.
class VoiceEffect
{
public:
    virtual ~VoiceEffect() = default;

    // Brings the chain up: loads and activates every enabled slot at 48 kHz
    // with a fixed 960-sample block. False means nothing was loaded and the
    // call should carry on dry; status().error says why. Safe to call again.
    [[nodiscard]] virtual bool prepare() = 0;

    // One 20 ms frame in, one out. Returns the input untouched when no slot is
    // enabled and when the chain has given up. A frame of the wrong length is
    // passed through untouched, for the same reason MicrophoneProcessor does
    // it: nothing downstream would accept it anyway.
    [[nodiscard]] virtual AudioFrame process(const AudioFrame &frame) = 0;

    // Drops every plugin's internal tail. Called when capture starts and when
    // the effect is detached, so a reverb from the last call -- or from before
    // a mute -- never opens the next one.
    virtual void reset() = 0;

    [[nodiscard]] virtual VoiceEffectStatus status() const = 0;
};

// How a call obtains its effect. Injected the way CallAudioIoFactory is, so a
// test can substitute a pure-C++ fake with no plugin at all, and so each call
// builds its own -- a chain that failed in one call does not carry into the
// next.
using VoiceEffectFactory = std::function<std::unique_ptr<VoiceEffect>()>;

} // namespace OpenChat
