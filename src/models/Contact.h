#pragma once

#include <QString>

namespace OpenChat {

enum class Presence {
    Available,
    Away,
    Offline,
};

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
    }
    return QStringLiteral("Offline");
}

} // namespace OpenChat
