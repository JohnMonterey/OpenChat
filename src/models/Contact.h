#pragma once

#include <QString>

namespace OpenChat {

// Wire and model values are stable: QML compares the integers directly and a
// peer's ProfileUpdate carries them, so new members are appended, never
// inserted.
enum class Presence {
    Available = 0,
    Away = 1,
    Offline = 2,
    Busy = 3,
};

inline constexpr int presenceCount = 4;

// The presence a user can choose for themselves. Offline here means "appear
// offline": the device may well be connected, but contacts are told otherwise.
[[nodiscard]] inline bool isSelectablePresence(int value)
{
    return value >= 0 && value < presenceCount;
}

struct Contact {
    QString id;
    QString name;
    Presence presence = Presence::Offline;
    bool favorite = false;
    QString avatarKey;
    // Optional row subtitle. When empty the current presence text is shown.
    QString statusText;
};

inline QString presenceText(Presence presence)
{
    switch (presence) {
    case Presence::Available:
        return QStringLiteral("Available");
    case Presence::Away:
        return QStringLiteral("Away");
    case Presence::Offline:
        return QStringLiteral("Offline");
    case Presence::Busy:
        return QStringLiteral("Busy");
    }
    return QStringLiteral("Offline");
}

} // namespace OpenChat
