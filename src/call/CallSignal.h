#pragma once

#include "call/CallTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"
#include "media/AudioCodec.h"

#include <QByteArray>

#include <optional>

namespace OpenChat {

// The control plane of a call: four small messages that set one up and tear it
// down. They travel as MLS application messages inside the conversation's group
// (EnvelopeMessageKind::CallSignal), so confidentiality, integrity and sender
// authentication all come from the ratchet — this codec only has to be
// defensive about shape, never about trust.
enum class CallSignalType : quint8 {
    Offer = 0,   // "call me"; carries the media secret both ends key from
    Ringing = 1, // "your offer reached my device and it is alerting"
    Answer = 2,  // accept or decline
    Hangup = 3,  // end an offered or live call, with a reason
};

// The shared secret an Offer carries. 32 bytes of CSPRNG output, from which both
// ends derive their per-direction media keys; it never leaves the MLS envelope.
inline constexpr int callSecretBytes = 32;

struct CallSignalMessage final {
    CallSignalType type = CallSignalType::Hangup;
    CallId callId = CallId::generate();

    // Offer only.
    QByteArray secret;
    AudioCodecKind codec = AudioCodecKind::Pcm;

    // Answer only.
    bool accepted = false;

    // Hangup only.
    CallEndReason reason = CallEndReason::None;

    [[nodiscard]] static CallSignalMessage offer(const CallId &callId, const QByteArray &secret,
                                                 AudioCodecKind codec);
    [[nodiscard]] static CallSignalMessage ringing(const CallId &callId);
    [[nodiscard]] static CallSignalMessage answer(const CallId &callId, bool accepted,
                                                  AudioCodecKind codec);
    [[nodiscard]] static CallSignalMessage hangup(const CallId &callId, CallEndReason reason);
};

// Serialises to a compact CBOR array. Decoding rejects anything that is not
// exactly the expected shape rather than filling in defaults, so a malformed or
// truncated signal can never be mistaken for a well-formed one.
[[nodiscard]] QByteArray encodeCallSignal(const CallSignalMessage &message);
[[nodiscard]] std::optional<CallSignalMessage> decodeCallSignal(QByteArrayView bytes);

// Generates a fresh 32-byte call secret from the system CSPRNG. Returns empty on
// failure, which the caller must treat as "cannot start a call" rather than
// falling back to a weaker source.
[[nodiscard]] QByteArray generateCallSecret();

} // namespace OpenChat
