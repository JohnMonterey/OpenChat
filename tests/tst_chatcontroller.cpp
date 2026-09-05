#include <QtTest>

#include "CallTestSupport.h"
#include "app/ContactRequestService.h"
#include "app/ProfileSession.h"
#include "call/CallSignal.h"
#include "call/SyncCallTransport.h"
#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "crypto/MlsClient.h"
#include "domain/ChatTypes.h"
#include "domain/Contact.h"
#include "models/Message.h"
#include "network/SyncEngine.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"

#include "domain/ProfileUpdate.h"
#include "render/AvatarStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include <memory>
#include <optional>

using OpenChat::ChatController;
using OpenChat::MessageListModel;

namespace {

// Minimal in-memory KeyVault (no OS keychain in this environment), as in the
// other live-session tests.
class InMemoryVault final : public OpenChat::KeyVault
{
public:
    using KeyVaultAvailability = OpenChat::KeyVaultAvailability;
    using KeyVaultError = OpenChat::KeyVaultError;
    using SecureBuffer = OpenChat::SecureBuffer;
    template <typename T> using Result = OpenChat::Result<T, KeyVaultError>;

    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }
    Result<SecureBuffer> readProfileKey(const OpenChat::ProfileId &) override
    {
        return read(m_databaseKey);
    }
    Result<SecureBuffer> createProfileKey(const OpenChat::ProfileId &) override
    {
        return create(m_databaseKey);
    }
    Result<void> deleteProfileKey(const OpenChat::ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void>::success();
    }
    Result<SecureBuffer> readDeviceWrappingKey(const OpenChat::ProfileId &) override
    {
        return read(m_wrappingKey);
    }
    Result<SecureBuffer> createDeviceWrappingKey(const OpenChat::ProfileId &) override
    {
        return create(m_wrappingKey);
    }
    Result<void> deleteDeviceWrappingKey(const OpenChat::ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void>::success();
    }

private:
    static Result<SecureBuffer> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer>::success(SecureBuffer::fromBytes(key->view()));
    }
    static Result<SecureBuffer> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

class FakeTransport final : public OpenChat::SyncTransport
{
public:
    bool isConnected() const override { return true; }
    void sendEnvelope(const OpenChat::CiphertextEnvelopeV1 &envelope) override
    {
        sent.append(envelope);
    }
    void sendDatagram(const OpenChat::CiphertextEnvelopeV1 &envelope) override
    {
        datagrams.append(envelope);
    }
    void acknowledge(const OpenChat::EnvelopeId &, quint64) override {}
    QVector<OpenChat::CiphertextEnvelopeV1> sent;
    QVector<OpenChat::CiphertextEnvelopeV1> datagrams;
};

QByteArray credentialFor(const OpenChat::DeviceId &device)
{
    QByteArray credential;
    credential.append(char{1});
    credential.append(device.bytes());
    credential.append(QByteArray(32, 'k'));
    return credential;
}

// A live profile with one Accepted peer whose 2-party MLS group both sides hold:
// exactly the state a completed contact handshake leaves behind.
struct LiveFixture final {
    QTemporaryDir dir;
    InMemoryVault vault;
    std::unique_ptr<OpenChat::ProfileSession> session;
    std::unique_ptr<FakeTransport> transport;
    std::unique_ptr<OpenChat::SqlCipherDatabase> peerDb;
    std::unique_ptr<OpenChat::CapturingMlsStateStore> peerCapture;
    std::unique_ptr<OpenChat::MlsClient> peer;
    OpenChat::AccountId peerAccount = OpenChat::AccountId::generate();
    OpenChat::DeviceId peerDevice = OpenChat::DeviceId::generate();
    OpenChat::ConversationId conversation = OpenChat::ConversationId::generate();
    quint64 sequence = 0;

    bool setUp()
    {
        using namespace OpenChat;
        if (!dir.isValid())
            return false;
        const auto profileId = ProfileId::generate();
        auto created = ProfileSession::create(profileId, vault,
                                              ProfilePaths::forProfile(dir.path(), profileId));
        if (!created.hasValue())
            return false;
        session = std::move(created).value();
        transport = std::make_unique<FakeTransport>();
        if (!session->startNetworking(*transport).hasValue())
            return false;

        auto peerOpened = SqlCipherDatabase::open(dir.filePath(QStringLiteral("peer.sqlite3")),
                                                  SecureBuffer::random(32));
        if (!peerOpened.hasValue())
            return false;
        peerDb = std::make_unique<SqlCipherDatabase>(std::move(peerOpened).value());
        peerCapture = std::make_unique<CapturingMlsStateStore>(*peerDb, ProfileId::generate());
        auto peerResult = MlsClient::create(credentialFor(peerDevice), peerCapture.get());
        if (!peerResult.hasValue())
            return false;
        peer = std::move(peerResult).value();

        auto keyPackage = peer->generateKeyPackage();
        if (!keyPackage.hasValue() || !session->mls()->createGroup(conversation).hasValue())
            return false;
        auto add = session->mls()->addMembers(conversation, {keyPackage.value()});
        if (!add.hasValue() || !session->persistMlsState().hasValue()
            || !peer->joinGroup(conversation, add.value().welcome).hasValue())
            return false;
        return true;
    }

    bool acceptPeer(const QString &handle)
    {
        using namespace OpenChat;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        ContactRecord record{peerAccount, handle, QString(), ContactState::PendingOutgoing,
                             conversation, now, now};
        record.peerDeviceId = peerDevice;
        if (!session->contacts()->recordOutgoingRequest(record).hasValue())
            return false;
        if (!session->contacts()->markAccepted(peerAccount, conversation, now).hasValue())
            return false;
        return session->chats()
            ->upsertConversation(ConversationRecord{conversation, conversation.bytes(), QString(),
                                                    ConversationKind::Direct, now})
            .hasValue();
    }

    // The peer sends `text` into the group and the relay delivers it to us.
    bool deliverFromPeer(const QString &text)
    {
        return deliverFromPeer(OpenChat::EnvelopeMessageKind::MlsPrivateMessage, text.toUtf8());
    }

    // The peer sends a control message of `kind` (its plaintext `payload`
    // encrypted under the group ratchet) and the relay delivers it to us.
    bool deliverFromPeer(OpenChat::EnvelopeMessageKind kind, const QByteArray &payload)
    {
        using namespace OpenChat;
        auto ciphertext = peer->encrypt(conversation, payload);
        if (!ciphertext.hasValue())
            return false;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const CiphertextEnvelopeV1 envelope{
            1,
            EnvelopeId::generate(),
            peerAccount,
            peerDevice,
            DeviceId::generate(),
            conversation,
            kind,
            now,
            now + 3'600'000,
            EnvelopeId::generate(),
            ciphertext.value().bytes,
            QCryptographicHash::hash(ciphertext.value().bytes, QCryptographicHash::Sha256),
            QByteArray(64, '\x03')};
        session->syncEngine()->handleEnvelope(envelope, ++sequence);
        return true;
    }

    ~LiveFixture()
    {
        if (session)
            session->lock();
    }
};

// Writes a small but real photo to `dir` and returns its path.
QString writePhoto(const QTemporaryDir &dir, const QString &name, const QColor &colour)
{
    QImage image(120, 90, QImage::Format_RGB32);
    image.fill(colour);
    const QString path = dir.filePath(name);
    if (!image.save(path, "PNG"))
        return {};
    return path;
}

// The last envelope of `kind` the fake transport saw, decrypted by the peer
// and decoded as a profile.
std::optional<OpenChat::ProfileUpdateMessage> lastProfileSentTo(LiveFixture &live)
{
    using namespace OpenChat;
    for (auto it = live.transport->sent.crbegin(); it != live.transport->sent.crend(); ++it) {
        if (it->messageKind != EnvelopeMessageKind::ProfileUpdate)
            continue;
        auto processed = live.peer->process(live.conversation, it->ciphertext);
        if (!processed.hasValue())
            return std::nullopt;
        return decodeProfileUpdate(processed.value().applicationData);
    }
    return std::nullopt;
}

int countProfileUpdates(const LiveFixture &live)
{
    int count = 0;
    for (const auto &envelope : live.transport->sent)
        if (envelope.messageKind == OpenChat::EnvelopeMessageKind::ProfileUpdate)
            ++count;
    return count;
}

} // namespace

class ChatControllerTest final : public QObject
{
    Q_OBJECT

private slots:
    void startsOnMichaelWithReferenceConversation()
    {
        ChatController controller;

        QCOMPARE(controller.currentContactName(), "Michael");
        QCOMPARE(controller.currentStatusText(), "Available");
        QCOMPARE(controller.currentAvatarKey(), "michael");
        QCOMPARE(controller.messages()->rowCount(), 5);
        QCOMPARE(controller.messages()->data(controller.messages()->index(0),
                                              MessageListModel::BodyRole).toString(),
                 "Hey Daniel!");
    }

    void localUserNameIsExposedToQml()
    {
        ChatController controller;
        QSignalSpy nameSpy(&controller, &ChatController::localUserNameChanged);

        controller.setLocalUserName("  Ada Lovelace  ");

        QCOMPARE(controller.localUserName(), QStringLiteral("Ada Lovelace"));
        QCOMPARE(nameSpy.count(), 1);
    }

    void whitespaceCannotSend()
    {
        ChatController controller;

        controller.setComposerText("   \t ");

        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        QCOMPARE(controller.messages()->rowCount(), 5);
    }

    void sendTrimsAppendsAndClears()
    {
        ChatController controller;
        controller.setComposerText("  A local message  ");
        const int previousCount = controller.messages()->rowCount();

        QVERIFY(controller.canSend());
        QVERIFY(controller.sendMessage());

        QCOMPARE(controller.messages()->rowCount(), previousCount + 1);
        QCOMPARE(controller.messages()->data(controller.messages()->index(previousCount),
                                              MessageListModel::BodyRole).toString(),
                 "A local message");
        QCOMPARE(controller.composerText(), QString());
        QVERIFY(!controller.canSend());
    }

    void selectionChangesHeaderAndConversation()
    {
        ChatController controller;

        QVERIFY(controller.selectContact("sarah"));

        QCOMPARE(controller.currentContactName(), "Sarah");
        QCOMPARE(controller.currentStatusText(), "Away");
        QCOMPARE(controller.currentAvatarKey(), "sarah");
        QCOMPARE(controller.messages()->rowCount(), 0);
    }

    void searchUpdatesVisibleCounts()
    {
        ChatController controller;

        controller.setSearchQuery("tom");

        QCOMPARE(controller.contacts()->rowCount(), 1);
        QCOMPARE(controller.contacts()->favoriteCount(), 0);
        QCOMPARE(controller.contacts()->regularCount(), 1);
        QCOMPARE(controller.searchQuery(), "tom");
    }

    void defaultsToReadyWithVisiblePlaintext()
    {
        ChatController controller;

        QCOMPARE(controller.sessionState(), ChatController::SessionState::Ready);
        QVERIFY(controller.plaintextVisible());
        QVERIFY(controller.sessionStateText().isEmpty());
        QVERIFY(controller.securityNoticeText().isEmpty());
        QCOMPARE(controller.messages()->rowCount(), 5);
    }

    void lockWithholdsPlaintextAndPreservesComposer()
    {
        ChatController controller;
        controller.setComposerText("draft that must survive a lock");
        QVERIFY(controller.canSend());

        QSignalSpy stateSpy(&controller, &ChatController::sessionStateChanged);
        controller.setSessionState(ChatController::SessionState::Locked);

        QCOMPARE(stateSpy.count(), 1);
        QVERIFY(!controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 0);
        QVERIFY(!controller.securityNoticeText().isEmpty());
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        QCOMPARE(controller.composerText(), "draft that must survive a lock");

        controller.setSessionState(ChatController::SessionState::Ready);
        QVERIFY(controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 5);
        QVERIFY(controller.canSend());
    }

    void offlineShowsHistoryButDefersSending()
    {
        ChatController controller;

        controller.setSessionState(ChatController::SessionState::Offline);
        QVERIFY(controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 5);
        QVERIFY(!controller.sessionStateText().isEmpty());

        controller.setComposerText("not sent while offline");
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        QCOMPARE(controller.messages()->rowCount(), 5);
    }

    void navigationDefaultsToChatAndTransitionsOnce()
    {
        ChatController controller;

        QCOMPARE(controller.navSection(), ChatController::NavSection::Chat);
        QCOMPARE(controller.chatUnreadCount(), 3);
        QCOMPARE(controller.callMissedCount(), 1);
        QCOMPARE(controller.callCount(), 0);

        QSignalSpy navSpy(&controller, &ChatController::navSectionChanged);

        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(controller.navSection(), ChatController::NavSection::Call);
        QCOMPARE(navSpy.count(), 1);

        // Re-selecting the current section is a no-op and emits nothing further.
        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(navSpy.count(), 1);

        controller.setNavSection(ChatController::NavSection::Settings);
        QCOMPARE(controller.navSection(), ChatController::NavSection::Settings);
        QCOMPARE(navSpy.count(), 2);

        controller.setNavSection(ChatController::NavSection::Chat);
        QCOMPARE(controller.navSection(), ChatController::NavSection::Chat);
        QCOMPARE(navSpy.count(), 3);
    }

    void settingsCategoriesDriveSelectionAndElements()
    {
        ChatController controller;

        const QStringList categories = controller.settingsCategories();
        QCOMPARE(categories.size(), 7);
        QCOMPARE(categories.first(), QStringLiteral("General"));

        // Defaults to the first category.
        QCOMPARE(controller.currentSettingsCategory(), 0);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("General"));
        QVERIFY(!controller.currentSettingsElements().isEmpty());

        QSignalSpy categorySpy(&controller,
                               &ChatController::currentSettingsCategoryChanged);

        // Selecting a different category updates the index, name, and elements
        // and emits exactly once.
        controller.setCurrentSettingsCategory(2);
        QCOMPARE(controller.currentSettingsCategory(), 2);
        QCOMPARE(categorySpy.count(), 1);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("Privacy"));
        const QStringList privacy = controller.currentSettingsElements();
        QCOMPARE(privacy, (QStringList{QStringLiteral("Read receipts"),
                                       QStringLiteral("Who can contact me"),
                                       QStringLiteral("Blocked contacts"),
                                       QStringLiteral("Typing indicators")}));

        // Re-selecting the same category is a no-op and emits nothing further.
        controller.setCurrentSettingsCategory(2);
        QCOMPARE(categorySpy.count(), 1);

        // Out-of-range selections are ignored, leaving the current selection.
        controller.setCurrentSettingsCategory(-1);
        controller.setCurrentSettingsCategory(99);
        QCOMPARE(controller.currentSettingsCategory(), 2);
        QCOMPARE(categorySpy.count(), 1);
    }

    void anInboundMessageAsksForADesktopNotification()
    {
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        OpenChat::ContactRequestService requests(*live.session, *live.session->syncEngine());

        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QSignalSpy notified(&controller,
                            &ChatController::messageNotificationRequested);

        // Our own messages are not announced back to us.
        controller.setComposerText(QStringLiteral("hello bob"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(notified.count(), 0);

        // An inbound message asks for a notification naming the chat, the
        // sender, what they wrote and their picture. It is asked for even
        // though this is the conversation on screen: whether the user is
        // looking at the window is not something the controller can know.
        QVERIFY(live.deliverFromPeer(QStringLiteral("hi back")));
        QCOMPARE(notified.count(), 1);
        const QList<QVariant> first = notified.at(0);
        QCOMPARE(first.at(0).toString(), controller.currentContactId());
        QCOMPARE(first.at(1).toString(), QStringLiteral("bob"));
        QCOMPARE(first.at(2).toString(), QStringLiteral("hi back"));
        QCOMPARE(first.at(3).toString(), QStringLiteral("userpfp_none"));

        // A state that withholds message plaintext from the interface also
        // withholds it from the desktop: the arrival is announced, the words
        // are not.
        controller.setSessionState(ChatController::SessionState::Locked);
        QVERIFY(live.deliverFromPeer(QStringLiteral("meet me at the docks")));
        QCOMPARE(notified.count(), 2);
        const QList<QVariant> second = notified.at(1);
        QCOMPARE(second.at(1).toString(), QStringLiteral("bob"));
        QVERIFY(second.at(2).toString().isEmpty());
    }

    void liveRosterReplacesMockAndRoutesMessagesThroughEngine()
    {
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        OpenChat::ContactRequestService requests(*live.session, *live.session->syncEngine());

        ChatController controller;
        QSignalSpy contactSpy(&controller, &ChatController::currentContactChanged);
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);

        // The mock roster is gone: the one Accepted contact is the chat, opened.
        QVERIFY(controller.isLive());
        QCOMPARE(controller.contacts()->rowCount(), 1);
        QVERIFY(controller.hasCurrentContact());
        QCOMPARE(controller.currentContactName(), QStringLiteral("bob"));
        QCOMPARE(controller.currentStatusText(), QStringLiteral("Offline"));
        QCOMPARE(controller.messages()->rowCount(), 0);
        QCOMPARE(controller.chatUnreadCount(), 0);
        QVERIFY(contactSpy.count() >= 1);

        // Sending encrypts through the engine to the peer device and shows the
        // durable row (Queued, with its stable id) rather than a local echo.
        controller.setComposerText(QStringLiteral("hello bob"));
        QVERIFY(controller.canSend());
        QVERIFY(controller.sendMessage());
        QCOMPARE(controller.composerText(), QString());
        QCOMPARE(controller.messages()->rowCount(), 1);
        const QModelIndex sentRow = controller.messages()->index(0);
        QCOMPARE(controller.messages()->data(sentRow, MessageListModel::BodyRole).toString(),
                 QStringLiteral("hello bob"));
        QCOMPARE(controller.messages()->data(sentRow, MessageListModel::DirectionRole).toInt(),
                 static_cast<int>(OpenChat::MessageDirection::Outgoing));
        QCOMPARE(controller.messages()->data(sentRow, MessageListModel::DeliveryStateRole).toInt(),
                 static_cast<int>(OpenChat::MessageDeliveryState::Sending));
        QVERIFY(!controller.messages()
                     ->data(sentRow, MessageListModel::StableIdRole)
                     .toString()
                     .isEmpty());
        QCOMPARE(live.transport->sent.size(), qsizetype(1));
        QCOMPARE(live.transport->sent.first().recipientDeviceId.bytes(), live.peerDevice.bytes());
        QCOMPARE(live.transport->sent.first().messageKind,
                 OpenChat::EnvelopeMessageKind::MlsPrivateMessage);
        // The peer really can read it.
        auto processed = live.peer->process(live.conversation,
                                            live.transport->sent.first().ciphertext);
        QVERIFY(processed.hasValue());
        QCOMPARE(processed.value().applicationData, QByteArray("hello bob"));

        // Relay acceptance advances the visible row to Sent.
        live.transport->onRelayAccepted(live.transport->sent.first().envelopeId, 7);
        QCOMPARE(controller.messages()->data(sentRow, MessageListModel::DeliveryStateRole).toInt(),
                 static_cast<int>(OpenChat::MessageDeliveryState::Sent));

        // An inbound message from the peer lands in the open conversation.
        QVERIFY(live.deliverFromPeer(QStringLiteral("hi back")));
        QCOMPARE(controller.messages()->rowCount(), 2);
        const QModelIndex inRow = controller.messages()->index(1);
        QCOMPARE(controller.messages()->data(inRow, MessageListModel::BodyRole).toString(),
                 QStringLiteral("hi back"));
        QCOMPARE(controller.messages()->data(inRow, MessageListModel::DirectionRole).toInt(),
                 static_cast<int>(OpenChat::MessageDirection::Incoming));
        QCOMPARE(controller.chatUnreadCount(), 0); // it was the open chat

        // History is durable: a fresh controller reloads both rows in order.
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(reloaded.messages()->rowCount(), 2);
        QCOMPARE(reloaded.messages()->data(reloaded.messages()->index(0),
                                           MessageListModel::BodyRole).toString(),
                 QStringLiteral("hello bob"));
        QCOMPARE(reloaded.messages()->data(reloaded.messages()->index(1),
                                           MessageListModel::BodyRole).toString(),
                 QStringLiteral("hi back"));
    }

    void liveEmptyRosterThenAcceptedContactOpensChat()
    {
        LiveFixture live;
        QVERIFY(live.setUp());
        OpenChat::ContactRequestService requests(*live.session, *live.session->syncEngine());

        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);

        // Nothing accepted yet: no chat, nothing to send to, no mock leakage.
        QCOMPARE(controller.contacts()->rowCount(), 0);
        QVERIFY(!controller.hasCurrentContact());
        QVERIFY(controller.currentContactName().isEmpty());
        controller.setComposerText(QStringLiteral("nobody to send to"));
        QVERIFY(!controller.canSend());
        QVERIFY(!controller.sendMessage());
        controller.setComposerText(QString());

        // The request service reports an acceptance (either side): the chat
        // appears and opens.
        QVERIFY(live.acceptPeer(QString()));
        emit requests.contactAccepted(live.peerAccount);
        QCOMPARE(controller.contacts()->rowCount(), 1);
        QVERIFY(controller.hasCurrentContact());
        QCOMPARE(controller.currentContactName(),
                 QStringLiteral("ID ") + live.peerAccount.toHex().left(10));

        // A handle resolved later renames the row.
        QVERIFY(live.session->contacts()->setHandle(live.peerAccount, QStringLiteral("carol"))
                    .hasValue());
        controller.refreshContact(live.peerAccount.toHex());
        QCOMPARE(controller.currentContactName(), QStringLiteral("carol"));

        // A message for a chat that is not open counts as unread until selected.
        ChatController other;
        other.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(other.chatUnreadCount(), 0);
        QVERIFY(live.deliverFromPeer(QStringLiteral("first")));
        // `other` has the chat open (it is the only one), so it reads it directly.
        QCOMPARE(other.messages()->rowCount(), 1);
    }

    void incomingCallUsesSavedContactIdentity_data()
    {
        QTest::addColumn<QString>("handle");
        QTest::addColumn<bool>("knownConversation");
        QTest::addColumn<bool>("knownDevice");
        QTest::newRow("saved caller") << QStringLiteral("Alice") << true << true;
        QTest::newRow("unresolved handle") << QString() << true << true;
        QTest::newRow("unknown conversation") << QStringLiteral("Alice") << false << true;
        QTest::newRow("different device") << QStringLiteral("Alice") << true << false;
    }

    void incomingCallUsesSavedContactIdentity()
    {
        using namespace OpenChat;
        QFETCH(QString, handle);
        QFETCH(bool, knownConversation);
        QFETCH(bool, knownDevice);
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(handle));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController chats;
        chats.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        const QString savedName = chats.currentContactName();
        chats.setSearchQuery(QStringLiteral("no matching contacts"));
        QCOMPARE(chats.contacts()->rowCount(), 0);

        SyncCallTransport transport(*live.session->syncEngine());
        CallTest::ScriptedAudioDevices devices;
        CallEngine engine(CallEngine::Config{}, transport, devices.factory());
        CallController calls;
        calls.setLiveEngine(&engine, &chats);
        QString nameAtAlert;
        connect(&calls, &CallController::incomingCall, this,
                [&] { nameAtAlert = calls.peerName(); });
        const bool knownCaller = knownConversation && knownDevice;
        const QString expected = knownCaller ? savedName : QStringLiteral("Unknown caller");

        // Exercise delivery through the live call transport, as a decrypted
        // offer arrives from SyncEngine. No microphone or network is needed.
        emit live.session->syncEngine()->callSignalReceived(
            knownConversation ? live.conversation : ConversationId::generate(),
            knownDevice ? live.peerDevice : DeviceId::generate(),
            encodeCallSignal(CallSignalMessage::offer(
                CallId::generate(), generateCallSecret(), AudioCodecKind::Pcm)));
        QVERIFY(calls.isRinging());
        QCOMPARE(nameAtAlert, expected);
        QCOMPARE(calls.peerName(), expected);
        QCOMPARE(calls.peerAvatarKey(), QStringLiteral("userpfp_none"));

        // Late handle resolution must update a ringing call even though the
        // incoming engine peer has no contactId or displayName of its own.
        QSignalSpy changed(&calls, &CallController::callChanged);
        QVERIFY(live.session->contacts()->setHandle(live.peerAccount, QStringLiteral("AliceNew"))
                    .hasValue());
        chats.refreshContact(live.peerAccount.toHex());
        QVERIFY(!changed.isEmpty());
        const QString updated = knownCaller ? QStringLiteral("AliceNew") : expected;
        QCOMPARE(calls.peerName(), updated);
        calls.acceptCall();
        QCOMPARE(engine.state(), CallState::Connecting);
        QCOMPARE(calls.peerName(), updated);
    }

    void mockProfileEditsUpdateTheSidebarWithoutServices()
    {
        ChatController controller;
        QSignalSpy profileSpy(&controller, &ChatController::localProfileChanged);
        QSignalSpy noticeSpy(&controller, &ChatController::profileNoticeChanged);

        // Defaults: Available, no custom status, the neutral picture.
        QCOMPARE(controller.localPresence(), 0);
        QVERIFY(controller.localStatusText().isEmpty());
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Available"));
        QCOMPARE(controller.localAvatarKey(), QStringLiteral("userpfp_none"));
        QVERIFY(controller.profileNotice().isEmpty());

        // The status line is trimmed, capped, and shown in place of the presence.
        controller.setLocalStatusText(QStringLiteral("  Out for lunch  "));
        QCOMPARE(controller.localStatusText(), QStringLiteral("Out for lunch"));
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Out for lunch"));
        QCOMPARE(profileSpy.count(), 1);
        controller.setLocalStatusText(QStringLiteral("Out for lunch"));
        QCOMPARE(profileSpy.count(), 1); // unchanged: no signal
        controller.setLocalStatusText(QString(500, QLatin1Char('z')));
        QCOMPARE(controller.localStatusText().size(), OpenChat::maxStatusTextLength);
        controller.setLocalStatusText(QString());
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Available"));

        // Presence must be a real value; Offline means "appear offline".
        controller.setLocalPresence(static_cast<int>(OpenChat::Presence::Busy));
        QCOMPARE(controller.localPresence(), 3);
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Busy"));
        controller.setLocalPresence(2);
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Offline"));
        controller.setLocalPresence(99);
        controller.setLocalPresence(-1);
        QCOMPARE(controller.localPresence(), 2);

        // A picture file is scaled into a content-keyed avatar; junk is refused
        // with a notice the sidebar can show, and clears again on request.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString photo = writePhoto(dir, QStringLiteral("me.png"), QColor("#35618f"));
        QVERIFY(!photo.isEmpty());
        QVERIFY(controller.setLocalAvatarFromFile(QUrl::fromLocalFile(photo)));
        QVERIFY(OpenChat::AvatarStore::isBlobKey(controller.localAvatarKey()));
        QVERIFY(OpenChat::AvatarStore::instance().image(controller.localAvatarKey()).has_value());
        QVERIFY(controller.profileNotice().isEmpty());
        const QString firstKey = controller.localAvatarKey();

        const QString junk = dir.filePath(QStringLiteral("junk.png"));
        {
            QFile file(junk);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("definitely not an image");
        }
        QVERIFY(!controller.setLocalAvatarFromFile(QUrl::fromLocalFile(junk)));
        QVERIFY(!controller.profileNotice().isEmpty());
        QCOMPARE(controller.localAvatarKey(), firstKey); // the old picture stays
        QVERIFY(noticeSpy.count() >= 1);
        controller.clearProfileNotice();
        QVERIFY(controller.profileNotice().isEmpty());

        // A different picture is a different key, so bindings refresh.
        const QString other = writePhoto(dir, QStringLiteral("other.png"), QColor("#e0503d"));
        QVERIFY(controller.setLocalAvatarFromFile(QUrl::fromLocalFile(other)));
        QVERIFY(controller.localAvatarKey() != firstKey);

        // The profile a contact would receive carries all three fields.
        const OpenChat::ProfileUpdateMessage profile = controller.localProfile();
        QCOMPARE(profile.presence, 2);
        QVERIFY(profile.statusText.isEmpty());
        QVERIFY(!profile.avatarJpeg.isEmpty());
        QCOMPARE(OpenChat::AvatarStore::keyFor(profile.avatarJpeg), controller.localAvatarKey());
    }

    void liveProfileEditsArePublishedAndPersisted()
    {
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        OpenChat::ContactRequestService requests(*live.session, *live.session->syncEngine());

        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(countProfileUpdates(live), 0);

        // Each edit publishes the WHOLE profile to the contact as a
        // ProfileUpdate the peer can decrypt, and nothing goes out as a
        // visible message.
        controller.setLocalStatusText(QStringLiteral("Shipping it"));
        QCOMPARE(countProfileUpdates(live), 1);
        QCOMPARE(live.transport->sent.size(), qsizetype(1));
        QCOMPARE(live.transport->sent.last().recipientDeviceId.bytes(), live.peerDevice.bytes());
        auto published = lastProfileSentTo(live);
        QVERIFY(published.has_value());
        QCOMPARE(published->statusText, QStringLiteral("Shipping it"));
        QCOMPARE(published->presence, 0);
        QVERIFY(published->avatarJpeg.isEmpty());
        QCOMPARE(controller.messages()->rowCount(), 0);

        controller.setLocalPresence(1);
        QCOMPARE(countProfileUpdates(live), 2);
        published = lastProfileSentTo(live);
        QVERIFY(published.has_value());
        QCOMPARE(published->presence, 1);
        QCOMPARE(published->statusText, QStringLiteral("Shipping it"));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString photo = writePhoto(dir, QStringLiteral("me.png"), QColor("#78acd3"));
        QVERIFY(controller.setLocalAvatarFromFile(QUrl::fromLocalFile(photo)));
        QCOMPARE(countProfileUpdates(live), 3);
        published = lastProfileSentTo(live);
        QVERIFY(published.has_value());
        QVERIFY(!published->avatarJpeg.isEmpty());
        QCOMPARE(OpenChat::AvatarStore::keyFor(published->avatarJpeg), controller.localAvatarKey());

        // Everything is on the profile, so a fresh controller (a restart)
        // comes back with the same picture, status and presence.
        QCOMPARE(live.session->statusText(), QStringLiteral("Shipping it"));
        QCOMPARE(live.session->presence(), 1);
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(reloaded.localStatusText(), QStringLiteral("Shipping it"));
        QCOMPARE(reloaded.localPresence(), 1);
        QCOMPARE(reloaded.localAvatarKey(), controller.localAvatarKey());
        QCOMPARE(countProfileUpdates(live), 3); // loading is not a change: nothing sent
    }

    void newlyAcceptedContactIsIntroducedToTheProfile()
    {
        LiveFixture live;
        QVERIFY(live.setUp());
        OpenChat::ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        controller.setLocalStatusText(QStringLiteral("Hello, new friend"));
        QCOMPARE(countProfileUpdates(live), 0); // nobody to tell yet

        // The moment a contact becomes a chat they get our current profile,
        // even though nothing changed on our side.
        QVERIFY(live.acceptPeer(QStringLiteral("carol")));
        emit requests.contactAccepted(live.peerAccount);
        QCOMPARE(countProfileUpdates(live), 1);
        auto published = lastProfileSentTo(live);
        QVERIFY(published.has_value());
        QCOMPARE(published->statusText, QStringLiteral("Hello, new friend"));
    }

    void inboundProfileUpdateChangesTheContactRow()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(controller.currentStatusText(), QStringLiteral("Offline"));
        QCOMPARE(controller.currentAvatarKey(), QStringLiteral("userpfp_none"));

        // Bob publishes a status, a presence and a picture.
        QImage picture(64, 64, QImage::Format_RGB32);
        picture.fill(QColor("#4e9f0f"));
        QByteArray jpeg;
        {
            QBuffer buffer(&jpeg);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QVERIFY(picture.save(&buffer, "JPEG", 80));
        }
        ProfileUpdateMessage theirs;
        theirs.presence = static_cast<int>(Presence::Busy);
        theirs.statusText = QStringLiteral("Deep in code");
        theirs.avatarJpeg = jpeg;
        QSignalSpy contactSpy(&controller, &ChatController::currentContactChanged);
        QVERIFY(live.deliverFromPeer(EnvelopeMessageKind::ProfileUpdate,
                                     encodeProfileUpdate(theirs)));

        // The row shows their words and picture. Their chosen presence is
        // remembered but only shown while their device is reachable, which
        // without a relay it never is: the bead stays Offline.
        QVERIFY(contactSpy.count() >= 1);
        QCOMPARE(controller.currentStatusText(), QStringLiteral("Deep in code"));
        QCOMPARE(controller.currentAvatarKey(), AvatarStore::keyFor(jpeg));
        QVERIFY(AvatarStore::instance().image(controller.currentAvatarKey()).has_value());
        QCOMPARE(controller.currentPresence(), static_cast<int>(Presence::Offline));
        QCOMPARE(controller.messages()->rowCount(), 0); // not conversation
        const QModelIndex row = controller.contacts()->index(0);
        QCOMPARE(controller.contacts()->data(row, OpenChat::ContactListModel::StatusTextRole)
                     .toString(),
                 QStringLiteral("Deep in code"));

        // It is durable: the roster row holds it, and a restart shows it again.
        auto stored = live.session->contacts()->find(live.peerAccount);
        QVERIFY(stored.hasValue() && stored.value().has_value());
        QCOMPARE(stored.value()->presence, static_cast<int>(Presence::Busy));
        QCOMPARE(stored.value()->statusText, QStringLiteral("Deep in code"));
        QCOMPARE(stored.value()->avatarJpeg, jpeg);
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(reloaded.currentStatusText(), QStringLiteral("Deep in code"));
        QCOMPARE(reloaded.currentAvatarKey(), AvatarStore::keyFor(jpeg));
        // And the call screen would show that picture for them.
        QCOMPARE(reloaded.callRouteFor(live.peerAccount.toHex())->avatarKey,
                 AvatarStore::keyFor(jpeg));

        // Clearing everything on their side clears it here.
        QVERIFY(live.deliverFromPeer(EnvelopeMessageKind::ProfileUpdate,
                                     encodeProfileUpdate(ProfileUpdateMessage{})));
        QCOMPARE(controller.currentStatusText(), QStringLiteral("Offline"));
        QCOMPARE(controller.currentAvatarKey(), QStringLiteral("userpfp_none"));

        // Garbage is ignored, not applied.
        QVERIFY(live.deliverFromPeer(EnvelopeMessageKind::ProfileUpdate, QByteArray("???")));
        QCOMPARE(controller.currentStatusText(), QStringLiteral("Offline"));
    }

    void quarantineAndDeviceChangeWithholdPlaintext()
    {
        ChatController controller;

        for (const auto state : {ChatController::SessionState::Quarantined,
                                 ChatController::SessionState::DeviceChanged}) {
            controller.setSessionState(state);
            QVERIFY(!controller.plaintextVisible());
            QCOMPARE(controller.messages()->rowCount(), 0);
            QVERIFY(!controller.securityNoticeText().isEmpty());
        }

        controller.setSessionState(ChatController::SessionState::Ready);
        QVERIFY(controller.plaintextVisible());
        QCOMPARE(controller.messages()->rowCount(), 5);
        QVERIFY(controller.securityNoticeText().isEmpty());
    }
};

QTEST_MAIN(ChatControllerTest)

#include "tst_chatcontroller.moc"
