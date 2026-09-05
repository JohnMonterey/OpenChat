#include "call/CallTypes.h"

namespace OpenChat {

QString callStateName(CallState state)
{
    switch (state) {
    case CallState::Idle:
        return QStringLiteral("Idle");
    case CallState::Dialing:
        return QStringLiteral("Dialing");
    case CallState::Ringing:
        return QStringLiteral("Ringing");
    case CallState::Connecting:
        return QStringLiteral("Connecting");
    case CallState::Active:
        return QStringLiteral("Active");
    case CallState::Ended:
        return QStringLiteral("Ended");
    }
    return QStringLiteral("Idle");
}

QString callEndReasonName(CallEndReason reason)
{
    switch (reason) {
    case CallEndReason::None:
        return QString();
    case CallEndReason::LocalHangup:
        return QStringLiteral("Call ended");
    case CallEndReason::RemoteHangup:
        return QStringLiteral("Call ended");
    case CallEndReason::Declined:
        return QStringLiteral("Declined");
    case CallEndReason::Busy:
        return QStringLiteral("Busy");
    case CallEndReason::NoAnswer:
        return QStringLiteral("No answer");
    case CallEndReason::Unanswered:
        return QStringLiteral("Missed call");
    case CallEndReason::SetupFailed:
        return QStringLiteral("Couldn't connect");
    case CallEndReason::TransportFailed:
        return QStringLiteral("Connection lost");
    case CallEndReason::Superseded:
        return QStringLiteral("Call ended");
    }
    return QString();
}

} // namespace OpenChat
