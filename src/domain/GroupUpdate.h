#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

#include <optional>

namespace OpenChat {

// Bounds on what a group chat carries about itself. The title is a single
// short line, like a status; the roster is bounded by what one MLS commit may
// add at once.
inline constexpr int maxGroupTitleLength = 80;
inline constexpr int maxGroupMemberNameLength = 80;
inline constexpr int maxGroupMembers = 256;

// One member of a group chat as the members describe each other: the account
// and the device that holds the group, plus the name the sender knows them by
// (a handle or display name; empty when unknown). Names are advisory: a member
// who is also a local contact is always shown under the local roster's name.
struct GroupMemberInfo final {
    AccountId account = AccountId::generate();
    DeviceId device = DeviceId::generate();
    QString name;

    friend bool operator==(const GroupMemberInfo &, const GroupMemberInfo &) = default;
};

enum class GroupUpdateType : quint8 {
    // The whole group: its title and every member (the sender included). Sent
    // to a newly invited member right after their Welcome, and to everyone when
    // a member is added, so each device converges on the same roster.
    Info = 0,
    // The title changed. Anyone in the group may rename it.
    Rename = 1,
    // The sender is leaving. The remaining members drop them from the roster,
    // and one of them commits their removal from the MLS group.
    Leave = 2,
};

// What one member tells the others about the group. Travels as an MLS
// application message inside the group (EnvelopeMessageKind::GroupControl), so
// confidentiality, integrity and sender authentication all come from the
// ratchet; this codec is defensive about shape only.
struct GroupUpdateMessage final {
    GroupUpdateType type = GroupUpdateType::Info;
    QString title;                    // Info and Rename
    QVector<GroupMemberInfo> members; // Info only

    [[nodiscard]] static GroupUpdateMessage info(const QString &title,
                                                 const QVector<GroupMemberInfo> &members);
    [[nodiscard]] static GroupUpdateMessage rename(const QString &title);
    [[nodiscard]] static GroupUpdateMessage leave();

    friend bool operator==(const GroupUpdateMessage &, const GroupUpdateMessage &) = default;
};

// Serialises to a fixed-arity CBOR array. Decoding rejects anything that is
// not exactly the expected shape or exceeds the bounds above rather than
// clamping, so a malformed update can never be mistaken for a well-formed one.
[[nodiscard]] QByteArray encodeGroupUpdate(const GroupUpdateMessage &message);
[[nodiscard]] std::optional<GroupUpdateMessage> decodeGroupUpdate(QByteArrayView bytes);

// Trims and caps a title the way every setter and the codec do, so the same
// text is stored, shown and sent everywhere.
[[nodiscard]] QString normalizeGroupTitle(const QString &title);

} // namespace OpenChat
