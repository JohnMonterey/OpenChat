#pragma once

#include "domain/ChatTypes.h"
#include "domain/GroupUpdate.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

namespace OpenChat {

class ProfileSession;
class RelayClient;
class SyncEngine;

// Group chats: creating one from Accepted contacts, adding a member, renaming,
// leaving, and applying the same changes when another member makes them.
//
// A group is a real multi-party MLS group whose id is the ConversationId. The
// creator claims one KeyPackage per invited device, adds them all in one commit
// and ships the Welcome (EnvelopeMessageKind::GroupWelcome) to each, followed
// by a GroupControl Info carrying the title and the full roster so every device
// converges on the same picture. A later add ships the Commit to the existing
// members and the Welcome to the newcomer in one atomic send. Leaving sends a
// GroupControl Leave; the remaining member with the lowest device id commits
// the leaver's removal so the group re-keys without them.
//
// Like ContactRequestService this is a durable QObject: it listens to the
// engine's group signals for the session's lifetime. KeyPackage claims are
// asynchronous (a relay round trip each), so create/add report their outcome
// through signals rather than return values.
//
// Lifetimes: the session and engine are borrowed and MUST outlive this object.
// The session must be unlocked with networking started.
class GroupService final : public QObject
{
    Q_OBJECT

public:
    // Claims one of `device`'s one-time KeyPackages and hands the bytes to
    // `done` (empty on failure). Injected so tests can answer claims without a
    // relay; relayClaimer() builds the real one.
    using KeyPackageClaimer =
        std::function<void(const DeviceId &device, std::function<void(const QByteArray &)> done)>;
    [[nodiscard]] static KeyPackageClaimer relayClaimer(RelayClient &relay);

    struct Group final {
        ConversationId conversation;
        QString title;
        QVector<GroupMemberRecord> members; // the OTHER members
        qint64 createdAtMs = 0;
    };

    GroupService(ProfileSession &session, SyncEngine &engine, KeyPackageClaimer claimer,
                 QObject *parent = nullptr);
    ~GroupService() override;

    GroupService(const GroupService &) = delete;
    GroupService &operator=(const GroupService &) = delete;

    // Every group this device is in and has not left, newest first.
    [[nodiscard]] QVector<Group> groups() const;
    [[nodiscard]] std::optional<Group> group(const ConversationId &conversation) const;
    // The devices a send into the group is addressed to.
    [[nodiscard]] QList<DeviceId> recipients(const ConversationId &conversation) const;

    // Starts a group with these Accepted contacts (each must have a known
    // device). Reports groupCreated or groupActionFailed once every KeyPackage
    // has been claimed. An empty title is allowed; the UI names it after the
    // members until someone renames it.
    void createGroup(const QList<AccountId> &members, const QString &title);
    // Adds one Accepted contact to an existing group.
    void addMember(const ConversationId &conversation, const AccountId &member);
    // Renames locally and tells every member. Returns false for an unknown group.
    bool rename(const ConversationId &conversation, const QString &title);
    // Tells every member, then hides the group here. Returns false if unknown.
    bool leave(const ConversationId &conversation);

    // Disconnect from the engine. Called by the destructor.
    void teardown();

signals:
    void groupCreated(const OpenChat::ConversationId &conversation);
    // Roster or title changed, by this device or another member.
    void groupChanged(const OpenChat::ConversationId &conversation);
    // Another member put this device into a group.
    void groupJoined(const OpenChat::ConversationId &conversation);
    // This device left (only ever from leave()).
    void groupLeft(const OpenChat::ConversationId &conversation);
    // A create or add could not be completed; `message` explains why.
    void groupActionFailed(const QString &message);

private:
    void logMembership(const ConversationId &conversation, const DeviceId &device,
                       const QString &name, bool joined);

    struct Invitee final {
        AccountId account;
        DeviceId device;
        QString name;
        QByteArray keyPackage;
    };
    // One in-flight create or add: the invitees whose KeyPackages are being
    // claimed one after another.
    struct PendingChange final {
        std::optional<ConversationId> conversation; // set for an add, unset for a create
        QString title;
        QVector<Invitee> invitees;
        int next = 0;
    };

    void onGroupWelcomeReceived(const ConversationId &conversation, const AccountId &senderAccount,
                                const DeviceId &senderDevice, const QList<QByteArray> &members);
    void onGroupControlReceived(const ConversationId &conversation, const DeviceId &senderDevice,
                                const QByteArray &payload);
    void applyInfo(const ConversationId &conversation, const GroupUpdateMessage &info);
    void applyLeave(const ConversationId &conversation, const DeviceId &leaver);
    void commitRemoval(const ConversationId &conversation, const DeviceId &leaver);

    [[nodiscard]] std::optional<Invitee> inviteeFor(const AccountId &account) const;
    void claimNext();
    void finishCreate(PendingChange &change);
    void finishAdd(PendingChange &change);
    [[nodiscard]] GroupUpdateMessage infoFor(const ConversationId &conversation,
                                             const QString &title) const;
    [[nodiscard]] std::optional<DeviceId> localDevice() const;
    [[nodiscard]] std::optional<AccountId> localAccount() const;
    [[nodiscard]] QString localName() const;
    [[nodiscard]] QString contactName(const AccountId &account) const;
    void fail(const QString &message);

    ProfileSession &m_session;
    SyncEngine &m_engine;
    KeyPackageClaimer m_claimer;
    std::unique_ptr<PendingChange> m_pending;
    QList<QMetaObject::Connection> m_connections;
};

} // namespace OpenChat
