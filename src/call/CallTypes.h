#pragma once

#include "domain/Identifiers.h"
#include "media/AudioCodec.h"

#include <QString>

namespace OpenChat {

// Which side placed the call. The direction decides which media key each end
// encrypts with, so it is part of the call's security state and not just a UI
// label.
enum class CallDirection {
    Outgoing,
    Incoming,
};

// The observable lifecycle of a call. Only Active carries media; every other
// state is a transition the UI renders differently.
enum class CallState {
    Idle,       // no call
    Dialing,    // we sent an offer, nothing came back yet
    Ringing,    // outgoing: the peer's device acknowledged; incoming: awaiting us
    Connecting, // answered, media keys agreed, first packet not yet exchanged
    Active,     // audio is flowing
    Ended,      // finished; endReason says why
};

// Why a call finished. Every path out of a live call sets exactly one of these,
// including the ones that are nobody's decision (timeouts, transport loss).
enum class CallEndReason {
    None,
    LocalHangup,
    RemoteHangup,
    Declined,
    Busy,            // the peer was already in a call
    NoAnswer,        // ring timeout elapsed
    Unanswered,      // we never answered an incoming call
    SetupFailed,     // keys/codec could not be agreed
    TransportFailed, // the link went away mid-call
    Superseded,      // lost a glare tie-break to the peer's simultaneous call
};

[[nodiscard]] QString callStateName(CallState state);
[[nodiscard]] QString callEndReasonName(CallEndReason reason);

// True in the states where a call occupies the device: a second incoming offer
// must be answered Busy rather than queued.
[[nodiscard]] constexpr bool callOccupiesDevice(CallState state) noexcept
{
    return state == CallState::Dialing || state == CallState::Ringing
        || state == CallState::Connecting || state == CallState::Active;
}

} // namespace OpenChat
