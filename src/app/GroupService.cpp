#include "app/GroupService.h"

#include <QJsonDocument>
#include <QJsonObject>

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/Contact.h"
#include "network/RelayClient.h"
#include "network/SyncEngine.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"

#include <QDateTime>
#include <QPointer>
#include <QQueue>

#include <algorithm>
#include <utility>

namespace OpenChat {

namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// A credential is version(1) || deviceId(16) || signingKey(32); the device is
// what a group roster is keyed by.
std::optional<DeviceId> deviceOfCredential(const QByteArray &credential)
{
    if (credential.size() < 1 + DeviceId::byteCount || credential.at(0) != 1)
        return std::nullopt;
    return DeviceId::fromBytes(credential.sliced(1, DeviceId::byteCount));
}

// Serialises KeyPackage claims over one RelayClient: the relay reports a claim's
// outcome through connection-wide signals with no request id, so only one may be
// in flight at a time. Owned by the std::function it is captured in; lives as
// long as the service holding that function.
class RelayKeyPackageClaimer final : public QObject
{
public:
    explicit RelayKeyPackageClaimer(RelayClient &relay)
        : m_relay(relay)
    {
        connect(&relay, &RelayClient::keyPackageClaimed, this,
                [this](const QByteArray &keyPackage) { finish(keyPackage); });
        connect(&relay, &RelayClient::keyPackageClaimFailed, this,
                [this](RelayClaimError) { finish({}); });
        connect(&relay, &RelayClient::authExpired, this, [this] { finish({}); });
        connect(&relay, &RelayClient::transportError, this,
                [this](RelayTransportError) { finish({}); });
    }

    void claim(const DeviceId &device, std::function<void(const QByteArray &)> done)
    {
        m_queue.enqueue({device, std::move(done)});
        if (m_queue.size() == 1)
            m_relay.claimKeyPackage(device);
    }

private:
    struct Request final {
        DeviceId device;
        std::function<void(const QByteArray &)> done;
    };

    void finish(const QByteArray &keyPackage)
    {
        if (m_queue.isEmpty())
            return;
        Request request = m_queue.dequeue();
        if (!m_queue.isEmpty())
            m_relay.claimKeyPackage(m_queue.head().device);
        if (request.done)
            request.done(keyPackage);
    }

    RelayClient &m_relay;
    QQueue<Request> m_queue;
};

} // namespace

GroupService::KeyPackageClaimer GroupService::relayClaimer(RelayClient &relay)
{
    auto claimer = std::make_shared<RelayKeyPackageClaimer>(relay);
    return [claimer](const DeviceId &device, std::function<void(const QByteArray &)> done) {
        claimer->claim(device, std::move(done));
    };
}

GroupService::GroupService(ProfileSession &session, SyncEngine &engine, KeyPackageClaimer claimer,
                           QObject *parent)
    : QObject(parent), m_session(session), m_engine(engine), m_claimer(std::move(claimer))
{
    m_connections << connect(&m_engine, &SyncEngine::groupWelcomeReceived, this,
                             &GroupService::onGroupWelcomeReceived);
    m_connections << connect(&m_engine, &SyncEngine::groupControlReceived, this,
                             &GroupService::onGroupControlReceived);
}

GroupService::~GroupService()
{
    teardown();
}

void GroupService::teardown()
{
    for (const QMetaObject::Connection &connection : m_connections)
        QObject::disconnect(connection);
    m_connections.clear();
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

QVector<GroupService::Group> GroupService::groups() const
{
    QVector<Group> result;
    SqlCipherChatRepository *chats = m_session.chats();
    if (chats == nullptr)
        return result;
    auto all = chats->conversations();
    if (!all.hasValue())
        return result;
    for (const ConversationRecord &record : all.value()) {
        if (record.kind != ConversationKind::Group || record.leftAtMs != 0)
            continue;
        Group group{record.id, record.title, {}, record.createdAtMs};
        if (auto members = chats->groupMembers(record.id); members.hasValue())
            group.members = std::move(members).value();
        result.append(std::move(group));
    }
    return result;
}

std::optional<GroupService::Group> GroupService::group(const ConversationId &conversation) const
{
    for (Group &group : groups())
        if (group.conversation == conversation)
            return std::move(group);
    return std::nullopt;
}

QList<DeviceId> GroupService::recipients(const ConversationId &conversation) const
{
    QList<DeviceId> devices;
    if (const auto found = group(conversation))
        for (const GroupMemberRecord &member : found->members)
            devices.append(member.deviceId);
    return devices;
}

std::optional<DeviceId> GroupService::localDevice() const
{
    const auto credential = m_session.publicCredential();
    if (!credential.hasValue())
        return std::nullopt;
    return credential.value().deviceId;
}

std::optional<AccountId> GroupService::localAccount() const
{
    const auto account = m_session.accountId();
    if (!account.hasValue())
        return std::nullopt;
    return account.value();
}

QString GroupService::localName() const
{
    return m_session.displayName();
}

QString GroupService::contactName(const AccountId &account) const
{
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (contacts == nullptr)
        return {};
    auto found = contacts->find(account);
    if (!found.hasValue() || !found.value().has_value())
        return {};
    return found.value()->handle.isEmpty() ? found.value()->displayName : found.value()->handle;
}

std::optional<GroupService::Invitee> GroupService::inviteeFor(const AccountId &account) const
{
    SqlCipherContactRepository *contacts = m_session.contacts();
    if (contacts == nullptr)
        return std::nullopt;
    auto found = contacts->find(account);
    if (!found.hasValue() || !found.value().has_value())
        return std::nullopt;
    const ContactRecord &record = *found.value();
    // Only a mutual contact with a known device can be put into a group: the
    // device is what the Welcome is sealed to and every envelope goes to.
    if (record.state != ContactState::Accepted || !record.peerDeviceId)
        return std::nullopt;
    return Invitee{record.accountId, *record.peerDeviceId,
                   record.handle.isEmpty() ? record.displayName : record.handle, {}};
}

GroupUpdateMessage GroupService::infoFor(const ConversationId &conversation,
                                         const QString &title) const
{
    QVector<GroupMemberInfo> members;
    if (const auto account = localAccount(); account)
        if (const auto device = localDevice(); device)
            members.append(GroupMemberInfo{*account, *device, localName()});
    if (const auto found = group(conversation)) {
        for (const GroupMemberRecord &member : found->members) {
            // Prefer our own roster's name for a member who is a contact: it is
            // fresher than whatever name was recorded when they were added.
            QString name = contactName(member.accountId);
            if (name.isEmpty())
                name = member.displayName;
            members.append(GroupMemberInfo{member.accountId, member.deviceId, name});
        }
    }
    return GroupUpdateMessage::info(title, members);
}

void GroupService::fail(const QString &message)
{
    m_pending.reset();
    emit groupActionFailed(message);
}

// ---------------------------------------------------------------------------
// Create / add: claim every KeyPackage, then commit the change in one go
// ---------------------------------------------------------------------------

void GroupService::createGroup(const QList<AccountId> &members, const QString &title)
{
    if (m_pending) {
        emit groupActionFailed(QStringLiteral("Another group change is still in progress."));
        return;
    }
    if (members.isEmpty()) {
        emit groupActionFailed(QStringLiteral("Pick at least one contact for the group."));
        return;
    }
    auto change = std::make_unique<PendingChange>();
    change->title = normalizeGroupTitle(title);
    for (const AccountId &account : members) {
        const auto invitee = inviteeFor(account);
        if (!invitee) {
            emit groupActionFailed(
                QStringLiteral("Only contacts who accepted you can be added to a group."));
            return;
        }
        const bool duplicate = std::any_of(
            change->invitees.cbegin(), change->invitees.cend(),
            [&](const Invitee &existing) { return existing.device == invitee->device; });
        if (!duplicate)
            change->invitees.append(*invitee);
    }
    m_pending = std::move(change);
    claimNext();
}

void GroupService::addMember(const ConversationId &conversation, const AccountId &member)
{
    if (m_pending) {
        emit groupActionFailed(QStringLiteral("Another group change is still in progress."));
        return;
    }
    const auto existing = group(conversation);
    if (!existing) {
        emit groupActionFailed(QStringLiteral("That group no longer exists."));
        return;
    }
    const auto invitee = inviteeFor(member);
    if (!invitee) {
        emit groupActionFailed(
            QStringLiteral("Only contacts who accepted you can be added to a group."));
        return;
    }
    const bool already = std::any_of(
        existing->members.cbegin(), existing->members.cend(),
        [&](const GroupMemberRecord &record) { return record.deviceId == invitee->device; });
    if (already || invitee->device == localDevice()) {
        emit groupActionFailed(QStringLiteral("They are already in this group."));
        return;
    }
    auto change = std::make_unique<PendingChange>();
    change->conversation = conversation;
    change->title = existing->title;
    change->invitees.append(*invitee);
    m_pending = std::move(change);
    claimNext();
}

void GroupService::claimNext()
{
    if (!m_pending)
        return;
    if (m_pending->next >= m_pending->invitees.size()) {
        // Every KeyPackage is in hand. The pending change is moved out first so
        // a re-entrant create/add from a signal handler starts clean.
        std::unique_ptr<PendingChange> change = std::move(m_pending);
        if (change->conversation)
            finishAdd(*change);
        else
            finishCreate(*change);
        return;
    }
    if (!m_claimer) {
        fail(QStringLiteral("Group chats need a connection to OpenChat."));
        return;
    }
    const Invitee &invitee = m_pending->invitees.at(m_pending->next);
    const DeviceId device = invitee.device;
    QPointer<GroupService> self(this);
    m_claimer(device, [self, device](const QByteArray &keyPackage) {
        if (!self || !self->m_pending)
            return;
        Invitee &current = self->m_pending->invitees[self->m_pending->next];
        if (current.device != device)
            return; // a stale answer for a change that was replaced
        if (keyPackage.isEmpty()) {
            const QString name = current.name.isEmpty() ? QStringLiteral("a member") : current.name;
            self->fail(QStringLiteral("Couldn't reach %1's device right now.").arg(name));
            return;
        }
        current.keyPackage = keyPackage;
        ++self->m_pending->next;
        self->claimNext();
    });
}

void GroupService::logMembership(const ConversationId &conversation, const DeviceId &device,
                                  const QString &name, bool joined)
{
    auto *chats = m_session.chats();
    if (chats == nullptr)
        return;
    const QString text = (name.isEmpty() ? QStringLiteral("Someone") : name)
        + (joined ? QStringLiteral(" Joined the group chat") : QStringLiteral(" Left the group chat"));
    MessageRecord record{MessageId::generate(), conversation, device, MessageFlow::Incoming,
        ContentKind::System, QString::fromUtf8(QJsonDocument(QJsonObject{{"event", "membership"},
        {"text", text}}).toJson(QJsonDocument::Compact)), nowMs(), DeliveryState::Sent, {}, {}};
    const auto saved = chats->saveEvent(record);
    if (saved.hasValue() && saved.value())
        emit m_engine.messageReceived(record);
}

void GroupService::finishCreate(PendingChange &change)
{
    // Runs to completion synchronously: the MLS state captured by createGroup +
    // addMembers is exactly what sendGroupChange commits with the Welcomes.
    MlsClient *mls = m_session.mls();
    SqlCipherChatRepository *chats = m_session.chats();
    if (mls == nullptr || chats == nullptr) {
        emit groupActionFailed(QStringLiteral("The group could not be saved."));
        return;
    }
    QList<QByteArray> keyPackages;
    QList<DeviceId> devices;
    for (const Invitee &invitee : change.invitees) {
        keyPackages.append(invitee.keyPackage);
        devices.append(invitee.device);
    }
    const ConversationId conversation = ConversationId::generate();
    if (!mls->createGroup(conversation).hasValue()) {
        emit groupActionFailed(QStringLiteral("The group could not be created."));
        return;
    }
    auto added = mls->addMembers(conversation, keyPackages);
    if (!added.hasValue()) {
        emit groupActionFailed(QStringLiteral("The group could not be created."));
        return;
    }
    // The durable rows come BEFORE the sends, in the same spirit as the contact
    // handshake: a crash here leaves a group row with no Welcome, which the user
    // can see and leave, rather than a Welcome nobody here remembers sending.
    const qint64 now = nowMs();
    if (!chats->upsertConversation(ConversationRecord{conversation, conversation.bytes(),
                                                      change.title, ConversationKind::Group, now})
             .hasValue()) {
        emit groupActionFailed(QStringLiteral("The group could not be saved."));
        return;
    }
    for (const Invitee &invitee : change.invitees) {
        (void)chats->upsertGroupMember(
            GroupMemberRecord{conversation, invitee.account, invitee.device, invitee.name, now});
    }
    // A brand-new group has no existing member to receive the Commit; only the
    // Welcomes ship, atomically with the group's epoch.
    m_engine.sendGroupChange(conversation, {}, {}, devices, added.value().welcome);
    m_engine.sendGroupControl(conversation, devices,
                              encodeGroupUpdate(infoFor(conversation, change.title)));
    emit groupCreated(conversation);
    if (const auto self = localDevice())
        logMembership(conversation, *self, localName(), true);
    for (const Invitee &invitee : change.invitees)
        logMembership(conversation, invitee.device, invitee.name, true);
}

void GroupService::finishAdd(PendingChange &change)
{
    MlsClient *mls = m_session.mls();
    SqlCipherChatRepository *chats = m_session.chats();
    const ConversationId conversation = *change.conversation;
    const auto existing = group(conversation);
    if (mls == nullptr || chats == nullptr || !existing || change.invitees.isEmpty()) {
        emit groupActionFailed(QStringLiteral("That group no longer exists."));
        return;
    }
    const Invitee &invitee = change.invitees.first();
    auto added = mls->addMembers(conversation, {invitee.keyPackage});
    if (!added.hasValue()) {
        emit groupActionFailed(QStringLiteral("They could not be added to the group."));
        return;
    }
    const QList<DeviceId> existingDevices = recipients(conversation);
    (void)chats->upsertGroupMember(
        GroupMemberRecord{conversation, invitee.account, invitee.device, invitee.name, nowMs()});
    // The Commit for everyone already in, the Welcome for the newcomer, one
    // atomic send with the new epoch; then the roster to all of them, encrypted
    // under that epoch, which the outbox delivers in this order.
    m_engine.sendGroupChange(conversation, existingDevices, added.value().commit,
                             {invitee.device}, added.value().welcome);
    QList<DeviceId> everyone = existingDevices;
    everyone.append(invitee.device);
    m_engine.sendGroupControl(conversation, everyone,
                              encodeGroupUpdate(infoFor(conversation, existing->title)));
    emit groupChanged(conversation);
    logMembership(conversation, invitee.device, invitee.name, true);
}

// ---------------------------------------------------------------------------
// Rename / leave
// ---------------------------------------------------------------------------

bool GroupService::rename(const ConversationId &conversation, const QString &title)
{
    SqlCipherChatRepository *chats = m_session.chats();
    const auto existing = group(conversation);
    if (chats == nullptr || !existing)
        return false;
    const QString normalized = normalizeGroupTitle(title);
    if (normalized == existing->title)
        return true;
    if (!chats->setConversationTitle(conversation, normalized).hasValue())
        return false;
    const QList<DeviceId> devices = recipients(conversation);
    if (!devices.isEmpty())
        m_engine.sendGroupControl(conversation, devices,
                                  encodeGroupUpdate(GroupUpdateMessage::rename(normalized)));
    emit groupChanged(conversation);
    return true;
}

bool GroupService::leave(const ConversationId &conversation)
{
    SqlCipherChatRepository *chats = m_session.chats();
    const auto existing = group(conversation);
    if (chats == nullptr || !existing)
        return false;
    const QList<DeviceId> devices = recipients(conversation);
    if (!devices.isEmpty())
        m_engine.sendGroupControl(conversation, devices,
                                  encodeGroupUpdate(GroupUpdateMessage::leave()));
    if (!chats->markConversationLeft(conversation, nowMs()).hasValue())
        return false;
    if (const auto self = localDevice())
        logMembership(conversation, *self, localName(), false);
    emit groupLeft(conversation);
    return true;
}

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

void GroupService::onGroupWelcomeReceived(const ConversationId &conversation,
                                          const AccountId &senderAccount,
                                          const DeviceId &senderDevice,
                                          const QList<QByteArray> &members)
{
    (void)members; // the roster with accounts and names follows in the Info
    SqlCipherChatRepository *chats = m_session.chats();
    if (chats == nullptr)
        return;
    // The engine committed the conversation row with the join. The inviter is
    // the one member whose account is known already; the rest arrive by Info.
    (void)chats->upsertGroupMember(GroupMemberRecord{conversation, senderAccount, senderDevice,
                                                     contactName(senderAccount), nowMs()});
    emit groupJoined(conversation);
    if (const auto self = localDevice())
        logMembership(conversation, *self, localName(), true);
    logMembership(conversation, senderDevice, contactName(senderAccount), true);
}

void GroupService::onGroupControlReceived(const ConversationId &conversation,
                                          const DeviceId &senderDevice, const QByteArray &payload)
{
    const auto update = decodeGroupUpdate(payload);
    if (!update)
        return;
    // A group this device already left is ignored: the remaining members simply
    // have not processed the Leave yet.
    if (!group(conversation))
        return;
    switch (update->type) {
    case GroupUpdateType::Info:
        applyInfo(conversation, *update);
        break;
    case GroupUpdateType::Rename:
        if (SqlCipherChatRepository *chats = m_session.chats())
            (void)chats->setConversationTitle(conversation, update->title);
        emit groupChanged(conversation);
        break;
    case GroupUpdateType::Leave:
        applyLeave(conversation, senderDevice);
        break;
    }
}

void GroupService::applyInfo(const ConversationId &conversation, const GroupUpdateMessage &info)
{
    SqlCipherChatRepository *chats = m_session.chats();
    const auto existing = group(conversation);
    if (chats == nullptr || !existing)
        return;
    const auto self = localDevice();
    (void)chats->setConversationTitle(conversation, info.title);
    // The Info is the sender's whole roster: rows it names are refreshed, rows
    // it does not name are gone (the sender saw them leave before we did).
    const qint64 now = nowMs();
    for (const GroupMemberInfo &member : info.members) {
        if (self && member.device == *self)
            continue;
        QString name = member.name;
        for (const GroupMemberRecord &record : existing->members)
            if (record.deviceId == member.device && !record.displayName.isEmpty()
                && name.isEmpty())
                name = record.displayName;
        const bool alreadyMember = std::any_of(existing->members.cbegin(), existing->members.cend(),
            [&](const GroupMemberRecord &record) { return record.deviceId == member.device; });
        if (chats->upsertGroupMember(
            GroupMemberRecord{conversation, member.account, member.device, name, now}).hasValue()
            && !alreadyMember)
            logMembership(conversation, member.device, name, true);
    }
    for (const GroupMemberRecord &record : existing->members) {
        const bool named = std::any_of(
            info.members.cbegin(), info.members.cend(),
            [&](const GroupMemberInfo &member) { return member.device == record.deviceId; });
        if (!named && chats->removeGroupMember(conversation, record.deviceId).hasValue())
            logMembership(conversation, record.deviceId, record.displayName, false);
    }
    emit groupChanged(conversation);
}

void GroupService::applyLeave(const ConversationId &conversation, const DeviceId &leaver)
{
    SqlCipherChatRepository *chats = m_session.chats();
    if (chats == nullptr)
        return;
    const auto existing = group(conversation);
    if (!existing)
        return;
    const auto member = std::find_if(existing->members.cbegin(), existing->members.cend(),
        [&](const GroupMemberRecord &record) { return record.deviceId == leaver; });
    if (member == existing->members.cend())
        return;
    if (!chats->removeGroupMember(conversation, leaver).hasValue())
        return;
    const QString knownName = contactName(member->accountId);
    logMembership(conversation, leaver, knownName.isEmpty() ? member->displayName : knownName, false);
    emit groupChanged(conversation);

    // Exactly one remaining member re-keys the group without the leaver: the
    // one with the lowest device id, a rule every member evaluates the same
    // way from the same roster. It runs on a later event-loop turn, outside
    // the engine's receive transaction that delivered the Leave, so the
    // removal commit and its send form one uninterrupted MLS step.
    const auto self = localDevice();
    if (!self)
        return;
    bool lowest = true;
    for (const DeviceId &device : recipients(conversation))
        if (device.bytes() < self->bytes())
            lowest = false;
    if (!lowest)
        return;
    QPointer<GroupService> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, conversation, leaver] {
            if (guard)
                guard->commitRemoval(conversation, leaver);
        },
        Qt::QueuedConnection);
}

void GroupService::commitRemoval(const ConversationId &conversation, const DeviceId &leaver)
{
    MlsClient *mls = m_session.mls();
    if (mls == nullptr || !group(conversation))
        return;
    auto members = mls->groupMembers(conversation);
    if (!members.hasValue())
        return;
    QList<QByteArray> identities;
    for (const QByteArray &credential : members.value())
        if (deviceOfCredential(credential) == leaver)
            identities.append(credential);
    if (identities.isEmpty())
        return; // already removed (another member committed first)
    auto commit = mls->removeMembers(conversation, identities);
    if (!commit.hasValue())
        return;
    m_engine.sendGroupChange(conversation, recipients(conversation), commit.value(), {}, {});
}

} // namespace OpenChat
