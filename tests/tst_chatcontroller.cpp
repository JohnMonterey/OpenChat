#include <QtTest>

#include "CallTestSupport.h"
#include "app/AppearanceSettings.h"
#include "app/ContactRequestService.h"
#include "app/MemorySettings.h"
#include "app/GroupService.h"
#include "app/ProfileSession.h"
#include "domain/GroupUpdate.h"
#include "domain/MessageContent.h"
#include "call/CallSignal.h"
#include "call/SyncCallTransport.h"
#include "controllers/CallController.h"
#include "controllers/ChatAttachmentMedia.h"
#include "controllers/ChatController.h"
#include "crypto/MlsClient.h"
#include "domain/ChatTypes.h"
#include "domain/Contact.h"
#include "models/Message.h"
#include "models/StagedAttachmentModel.h"
#include "network/SyncEngine.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/CapturingMlsStateStore.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"

#include "domain/ProfileUpdate.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfilePanelMedia.h"
#include "profile/ProfileRenderPolicy.h"
#include "profile/SongPlayer.h"
#include "security/AttachmentSeal.h"
#include "render/AvatarStore.h"

#include "app/ProfilePageSync.h"
#include "controllers/ProfileController.h"
#include "controllers/ProfilePageObject.h"
#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"

#include <QBuffer>
#include <QClipboard>
#include <QMimeData>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMetaProperty>
#include <QSettings>
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
    // What decryptedProfilePayloads() already opened, by index into sent.
    QHash<qsizetype, QByteArray> profilePlaintexts;

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

// Writes `bytes` to `dir` as `name` and returns its path.
QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    QFile file(dir.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return {};
    return file.fileName();
}

// Bytes nobody would guess, the same every run.
QByteArray patternBytes(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    quint32 state = 2463534242u;
    for (qsizetype index = 0; index < size; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = char(state & 0xFF);
    }
    return bytes;
}

// The row of the open chat whose `role` is `value`, or -1.
int rowWhere(const MessageListModel *messages, int role, const QVariant &value)
{
    for (int row = 0; row < messages->rowCount(); ++row) {
        if (messages->data(messages->index(row), role) == value)
            return row;
    }
    return -1;
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

// Every ProfileUpdate envelope the fake transport saw, as the peer decrypts
// it. An MLS ciphertext opens only once, so each envelope is decrypted the
// first time it is seen and its plaintext kept; a failed decryption is an
// empty entry. Tests that use this must not also use lastProfileSentTo().
QVector<QByteArray> decryptedProfilePayloads(LiveFixture &live)
{
    using namespace OpenChat;
    QVector<QByteArray> payloads;
    for (qsizetype index = 0; index < live.transport->sent.size(); ++index) {
        const CiphertextEnvelopeV1 &envelope = live.transport->sent.at(index);
        if (envelope.messageKind != EnvelopeMessageKind::ProfileUpdate)
            continue;
        if (!live.profilePlaintexts.contains(index)) {
            auto processed = live.peer->process(live.conversation, envelope.ciphertext);
            live.profilePlaintexts.insert(index, processed.hasValue() ? processed.value().applicationData
                                                                      : QByteArray());
        }
        payloads.append(live.profilePlaintexts.value(index));
    }
    return payloads;
}

int countProfileUpdates(const LiveFixture &live)
{
    int count = 0;
    for (const auto &envelope : live.transport->sent)
        if (envelope.messageKind == OpenChat::EnvelopeMessageKind::ProfileUpdate)
            ++count;
    return count;
}

// A second Accepted contact with its own MLS client, so a group can be made
// from two people. Its 2-party conversation with us is recorded but never used.
struct ExtraPeer final {
    std::unique_ptr<OpenChat::SqlCipherDatabase> db;
    std::unique_ptr<OpenChat::CapturingMlsStateStore> capture;
    std::unique_ptr<OpenChat::MlsClient> mls;
    OpenChat::AccountId account = OpenChat::AccountId::generate();
    OpenChat::DeviceId device = OpenChat::DeviceId::generate();

    bool setUp(LiveFixture &live, const QString &handle)
    {
        using namespace OpenChat;
        auto opened = SqlCipherDatabase::open(live.dir.filePath(handle + QStringLiteral(".sqlite3")),
                                              SecureBuffer::random(32));
        if (!opened.hasValue())
            return false;
        db = std::make_unique<SqlCipherDatabase>(std::move(opened).value());
        capture = std::make_unique<CapturingMlsStateStore>(*db, ProfileId::generate());
        auto client = MlsClient::create(credentialFor(device), capture.get());
        if (!client.hasValue())
            return false;
        mls = std::move(client).value();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const ConversationId direct = ConversationId::generate();
        ContactRecord record{account, handle, QString(), ContactState::PendingOutgoing, direct,
                             now, now};
        record.peerDeviceId = device;
        return live.session->contacts()->recordOutgoingRequest(record).hasValue()
            && live.session->contacts()->markAccepted(account, direct, now).hasValue()
            && live.session->chats()
                   ->upsertConversation(ConversationRecord{direct, direct.bytes(), QString(),
                                                           ConversationKind::Direct, now})
                   .hasValue();
    }
};

// Delivers one envelope of `kind` from `device`/`account` into `conversation`.
void deliver(LiveFixture &live, const OpenChat::AccountId &account, const OpenChat::DeviceId &device,
             const OpenChat::ConversationId &conversation, OpenChat::EnvelopeMessageKind kind,
             const QByteArray &ciphertext)
{
    using namespace OpenChat;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const CiphertextEnvelopeV1 envelope{
        1,
        EnvelopeId::generate(),
        account,
        device,
        DeviceId::generate(),
        conversation,
        kind,
        now,
        now + 3'600'000,
        EnvelopeId::generate(),
        ciphertext,
        QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256),
        QByteArray(64, '\x03')};
    live.session->syncEngine()->handleEnvelope(envelope, ++live.sequence);
}

// The envelopes of `kind` the transport saw addressed to `device`, in order.
QVector<OpenChat::CiphertextEnvelopeV1> sentTo(const LiveFixture &live, const OpenChat::DeviceId &device,
                                               OpenChat::EnvelopeMessageKind kind)
{
    QVector<OpenChat::CiphertextEnvelopeV1> result;
    for (const auto &envelope : live.transport->sent)
        if (envelope.messageKind == kind && envelope.recipientDeviceId == device)
            result.append(envelope);
    return result;
}

} // namespace

class ChatControllerTest final : public QObject
{
    Q_OBJECT

    // What the controllers and settings objects remember (appearance, recent
    // colours, call preferences) lives here for the run, never in a
    // developer's own settings.
    QTemporaryDir m_settingsDirectory;

private slots:
    void initTestCase()
    {
        QVERIFY(m_settingsDirectory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDirectory.path());

        // Every live controller here also runs profile page sync. Its
        // background page requests (production: up to two minutes after
        // start, under a minute after an acceptance) are pushed an hour out,
        // so the ProfileUpdate counts below are only what each test does.
        OpenChat::ProfilePageSync::Limits limits;
        limits.acceptRequestDelayMs = 60LL * 60 * 1000;
        limits.acceptRequestJitterMs = 1;
        limits.startupRequestJitterMs = 60LL * 60 * 1000;
        limits.missingMediaGraceMs = 60LL * 60 * 1000;
        OpenChat::ProfileController::setSyncTuningForTesting(
            OpenChat::ProfileController::SyncTuning{{}, [](qint64 bound) { return bound - 1; }, limits});
    }

    void cleanupTestCase()
    {
        OpenChat::ProfileController::setSyncTuningForTesting(std::nullopt);
    }

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
        QCOMPARE(controller.callMissedCount(), 0);
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

    void missedCallsCountUntilTheCallSectionOpens()
    {
        ChatController controller;
        QSignalSpy countSpy(&controller, &ChatController::callMissedCountChanged);

        controller.noteMissedCall();
        controller.noteMissedCall();
        QCOMPARE(controller.callMissedCount(), 2);
        QCOMPARE(countSpy.count(), 2);

        // Other sections leave the count alone.
        controller.setNavSection(ChatController::NavSection::Settings);
        controller.setNavSection(ChatController::NavSection::Chat);
        QCOMPARE(controller.callMissedCount(), 2);

        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(controller.callMissedCount(), 0);
        QCOMPARE(countSpy.count(), 3);

        // A call missed while the Call section is open is cleared by opening it
        // again, and clearing nothing says nothing.
        controller.noteMissedCall();
        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(controller.callMissedCount(), 0);
        QCOMPARE(countSpy.count(), 5);
        controller.setNavSection(ChatController::NavSection::Call);
        QCOMPARE(countSpy.count(), 5);
    }

    void composerTextStopsAtTheLengthLimit()
    {
        ChatController controller;
        const int max = ChatController::maxComposerLength;
        QCOMPARE(controller.composerMaxLength(), max);
        QSignalSpy textSpy(&controller, &ChatController::composerTextChanged);

        controller.setComposerText(QString(max + 100, QLatin1Char('a')));
        QCOMPARE(controller.composerText(), QString(max, QLatin1Char('a')));
        QCOMPARE(textSpy.count(), 1);
        // Offered too much again, it keeps what it has but says so, so a field
        // still showing the longer text takes the kept part back.
        controller.setComposerText(QString(max + 5, QLatin1Char('a')));
        QCOMPARE(controller.composerText().size(), max);
        QCOMPARE(textSpy.count(), 2);

        // A character made of two code units is dropped whole, not split.
        controller.setComposerText(QString(max - 1, QLatin1Char('b')) + QStringLiteral("\U0001F600"));
        QCOMPARE(controller.composerText(), QString(max - 1, QLatin1Char('b')));
    }

    void settingsCategoriesDriveSelectionAndElements()
    {
        ChatController controller;

        // Only categories with working controls in them.
        const QStringList categories = controller.settingsCategories();
        QCOMPARE(categories, (QStringList{QStringLiteral("General"),
                                          QStringLiteral("Audio & Video"),
                                          QStringLiteral("Appearance"),
                                          QStringLiteral("Cosmetics")}));

        // Defaults to the first category.
        QCOMPARE(controller.currentSettingsCategory(), 0);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("General"));
        QCOMPARE(controller.currentSettingsElements(), QStringList{QStringLiteral("Memory")});

        QSignalSpy categorySpy(&controller,
                               &ChatController::currentSettingsCategoryChanged);

        // Selecting a different category updates the index, name, and elements
        // and emits exactly once.
        controller.setCurrentSettingsCategory(1);
        QCOMPARE(controller.currentSettingsCategory(), 1);
        QCOMPARE(categorySpy.count(), 1);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("Audio & Video"));
        QCOMPARE(controller.currentSettingsElements(),
                 (QStringList{QStringLiteral("Input"), QStringLiteral("Custom Vocal FX"),
                              QStringLiteral("Connection")}));
        // Appearance: the app's own look, then how other people's profiles show.
        controller.setCurrentSettingsCategory(2);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("Appearance"));
        QCOMPARE(controller.currentSettingsElements(),
                 (QStringList{QStringLiteral("Theme"), QStringLiteral("Profiles")}));
        QCOMPARE(categorySpy.count(), 2);

        // Re-selecting the same category is a no-op and emits nothing further.
        controller.setCurrentSettingsCategory(2);
        QCOMPARE(categorySpy.count(), 2);

        // Cosmetics holds one picker per kind of collectible.
        controller.setCurrentSettingsCategory(3);
        QCOMPARE(controller.currentSettingsCategoryName(), QStringLiteral("Cosmetics"));
        QCOMPARE(controller.currentSettingsElements(),
                 (QStringList{QStringLiteral("Avatar frame"), QStringLiteral("Name flair"),
                              QStringLiteral("Presence bead"), QStringLiteral("Profile scene"),
                              QStringLiteral("Chat bubble")}));
        QCOMPARE(categorySpy.count(), 3);

        // Out-of-range selections are ignored, leaving the current selection.
        controller.setCurrentSettingsCategory(-1);
        controller.setCurrentSettingsCategory(4);
        controller.setCurrentSettingsCategory(99);
        QCOMPARE(controller.currentSettingsCategory(), 3);
        QCOMPARE(categorySpy.count(), 3);
    }

    // Appearance › Profiles and a profile's "Plain style" switch are one
    // remembered choice, off until the user turns it on.
    void plainProfilesIsARememberedAppearanceChoice()
    {
        using OpenChat::AppearanceSettings;
        const QString key = QStringLiteral("Appearance/plainProfiles");
        QSettings().remove(key);

        AppearanceSettings appearance;
        QVERIFY(!appearance.plainProfiles());
        const bool dark = appearance.darkMode();
        QSignalSpy changed(&appearance, &AppearanceSettings::plainProfilesChanged);
        appearance.setPlainProfiles(true);
        QVERIFY(appearance.plainProfiles());
        QCOMPARE(changed.count(), 1);
        appearance.setPlainProfiles(true);
        QCOMPARE(changed.count(), 1);
        QVERIFY(QSettings().value(key).toBool());
        // Its own setting: the app's look is untouched.
        QCOMPARE(appearance.darkMode(), dark);

        // The next start reads it back.
        AppearanceSettings restarted;
        QVERIFY(restarted.plainProfiles());

        // QML reads and writes it by name, and hears it change.
        const QMetaObject &meta = AppearanceSettings::staticMetaObject;
        const QMetaProperty property = meta.property(meta.indexOfProperty("plainProfiles"));
        QVERIFY(property.isValid());
        QVERIFY(property.isWritable());
        QVERIFY(property.hasNotifySignal());
        QCOMPARE(property.notifySignal().name(), QByteArray("plainProfilesChanged"));
        QSignalSpy restartedChanged(&restarted, &AppearanceSettings::plainProfilesChanged);
        QVERIFY(property.write(&restarted, false));
        QVERIFY(!restarted.plainProfiles());
        QCOMPARE(restartedChanged.count(), 1);
        QVERIFY(!QSettings().value(key).toBool());
        QVERIFY(!AppearanceSettings().plainProfiles());
    }

    // Low memory mode reaches the profile renderer at once, both ways: its
    // animations hold still and a page's background picture is not kept
    // decoded. A start with the mode saved on applies it before any page.
    void lowMemoryModeReachesProfilePages()
    {
        using namespace OpenChat;
        const auto restore = qScopeGuard([] {
            QSettings().remove(QStringLiteral("Performance/lowMemoryMode"));
            ProfileRenderPolicy::instance().resetForTesting();
            ProfileMediaStore::instance().resetForTesting();
        });
        QSettings().remove(QStringLiteral("Performance/lowMemoryMode"));
        ProfileRenderPolicy::instance().resetForTesting();
        ProfileMediaStore::instance().resetForTesting();
        QVERIFY(!ProfileRenderPolicy::instance().lowMemoryMode());
        QVERIFY(ProfileMediaStore::instance().keepDecoded());

        {
            MemorySettings memory;
            QVERIFY(!ProfileRenderPolicy::instance().lowMemoryMode());
            QVERIFY(ProfileMediaStore::instance().keepDecoded());
            memory.setLowMemoryMode(true);
            QVERIFY(ProfileRenderPolicy::instance().lowMemoryMode());
            QVERIFY(!ProfileRenderPolicy::instance().animationsAllowed());
            QVERIFY(!ProfileMediaStore::instance().keepDecoded());
            memory.setLowMemoryMode(false);
            QVERIFY(!ProfileRenderPolicy::instance().lowMemoryMode());
            QVERIFY(ProfileMediaStore::instance().keepDecoded());
            memory.setLowMemoryMode(true);
        }

        ProfileRenderPolicy::instance().resetForTesting();
        ProfileMediaStore::instance().resetForTesting();
        const MemorySettings restarted;
        QVERIFY(restarted.lowMemoryMode());
        QVERIFY(ProfileRenderPolicy::instance().lowMemoryMode());
        QVERIFY(!ProfileMediaStore::instance().keepDecoded());
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

    void unreadBadgesPersistAndChatsFollowLatestMessage()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ExtraPeer carol;
        QVERIFY(carol.setUp(live, QStringLiteral("carol")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setConversationVisible(false);
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QVERIFY(controller.selectContact(carol.account.toHex()));
        QVERIFY(live.deliverFromPeer(QStringLiteral("one")));
        QVERIFY(live.deliverFromPeer(QStringLiteral("two")));
        QCOMPARE(controller.currentContactId(), carol.account.toHex());
        QCOMPARE(controller.contacts()->contactAt(0)->id, live.peerAccount.toHex());
        QCOMPARE(controller.contacts()->contactAt(0)->unreadCount, 2);
        QCOMPARE(controller.chatUnreadCount(), 2);
        // Reloading the roster preserves both activity and the unread count.
        controller.refreshContact(live.peerAccount.toHex());
        QCOMPARE(controller.contacts()->contactAt(0)->unreadCount, 2);
        ChatController reloaded;
        reloaded.setConversationVisible(false);
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(reloaded.contacts()->contactAt(0)->id, live.peerAccount.toHex());
        QCOMPARE(reloaded.chatUnreadCount(), 2);
        QVERIFY(reloaded.selectContact(live.peerAccount.toHex()));
        QCOMPARE(reloaded.chatUnreadCount(), 2); // hidden windows do not read
        reloaded.setNavSection(ChatController::NavSection::Settings);
        reloaded.setConversationVisible(true);
        QCOMPARE(reloaded.chatUnreadCount(), 2);
        reloaded.setNavSection(ChatController::NavSection::Chat);
        QCOMPARE(reloaded.chatUnreadCount(), 0);
        QCOMPARE(reloaded.contacts()->contactAt(0)->unreadCount, 0);
        reloaded.setSessionState(ChatController::SessionState::Locked);
        QVERIFY(live.deliverFromPeer(QStringLiteral("while locked")));
        QCOMPARE(reloaded.chatUnreadCount(), 1);
        reloaded.setSessionState(ChatController::SessionState::Ready);
        QCOMPARE(reloaded.chatUnreadCount(), 0);
        // Sending from the selected chat also updates its position.
        reloaded.setComposerText(QStringLiteral("reply"));
        QVERIFY(reloaded.sendMessage());
        QCOMPARE(reloaded.contacts()->contactAt(0)->id, live.peerAccount.toHex());
        QCOMPARE(reloaded.contacts()->contactAt(0)->unreadCount, 0);
    }

    void callEventsAreDirectionalDurableAndDeduplicated()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        const auto route = controller.callRouteFor(controller.currentContactId());
        QVERIFY(route);
        const auto offer = encodeCallSignal(CallSignalMessage::offer(
            CallId::generate(), generateCallSecret(), AudioCodecKind::Pcm));
        auto *engine = live.session->syncEngine();
        engine->sendCallSignal(route->conversation, live.peerDevice, offer);
        engine->sendCallSignal(route->conversation, live.peerDevice, offer);
        QCOMPARE(controller.messages()->rowCount(), 1);
        QCOMPARE(controller.messages()->messageAt(0)->kind, MessageKind::CallEvent);
        QCOMPARE(controller.messages()->messageAt(0)->direction, MessageDirection::Outgoing);
        QVERIFY(controller.messages()->messageAt(0)->body.endsWith(QStringLiteral(" Started a call")));
        const auto incoming = encodeCallSignal(CallSignalMessage::offer(
            CallId::generate(), generateCallSecret(), AudioCodecKind::Pcm));
        // The peer must consume both outbound signals before encrypting its offer.
        for (const auto &envelope : sentTo(live, live.peerDevice, EnvelopeMessageKind::CallSignal))
            QVERIFY(live.peer->process(route->conversation, envelope.ciphertext).hasValue());
        const auto encrypted = live.peer->encrypt(route->conversation, incoming);
        QVERIFY(encrypted.hasValue());
        deliver(live, live.peerAccount, live.peerDevice, route->conversation,
                EnvelopeMessageKind::CallSignal, encrypted.value().bytes);
        QCOMPARE(controller.messages()->rowCount(), 2);
        QCOMPARE(controller.messages()->messageAt(1)->direction, MessageDirection::Incoming);
        QCOMPARE(controller.messages()->messageAt(1)->body, QStringLiteral("bob Started a call"));
        // A second envelope with the same call ID still produces only one row.
        const auto repeated = live.peer->encrypt(route->conversation, incoming);
        QVERIFY(repeated.hasValue());
        deliver(live, live.peerAccount, live.peerDevice, route->conversation,
                EnvelopeMessageKind::CallSignal, repeated.value().bytes);
        QCOMPARE(controller.messages()->rowCount(), 2);
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), engine, &requests);
        QCOMPARE(reloaded.messages()->rowCount(), 2);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(reloaded.messages()->messageAt(i)->kind, MessageKind::CallEvent);
            QCOMPARE(reloaded.messages()->messageAt(i)->deliveryState, MessageDeliveryState::None);
        }
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

    void mockEditReplyAndCopyWorkOnScreen()
    {
        ChatController controller;
        MessageListModel *messages = controller.messages();
        const auto role = [messages](int row, int role) {
            return messages->data(messages->index(row), role);
        };
        const QString theirs = role(0, MessageListModel::StableIdRole).toString();
        const QString mine = role(1, MessageListModel::StableIdRole).toString();
        QVERIFY(!theirs.isEmpty() && !mine.isEmpty());
        QVERIFY(!role(0, MessageListModel::EditableRole).toBool());
        QVERIFY(role(1, MessageListModel::EditableRole).toBool());
        QSignalSpy modeSpy(&controller, &ChatController::composeModeChanged);

        // Only one's own messages can be edited.
        QVERIFY(!controller.beginEdit(theirs));
        QVERIFY(controller.editingMessageId().isEmpty());

        // The edit borrows the composer and gives back what was typed there.
        controller.setComposerText(QStringLiteral("half a thought"));
        QVERIFY(controller.beginEdit(mine));
        QCOMPARE(controller.editingMessageId(), mine);
        QCOMPARE(controller.composerText(), role(1, MessageListModel::BodyRole).toString());
        controller.setComposerText(QStringLiteral("Hey Michael, how are you?"));
        const int rows = messages->rowCount();
        QVERIFY(controller.sendMessage());
        QCOMPARE(messages->rowCount(), rows);
        QCOMPARE(role(1, MessageListModel::BodyRole).toString(),
                 QStringLiteral("Hey Michael, how are you?"));
        QVERIFY(role(1, MessageListModel::EditedRole).toBool());
        QVERIFY(controller.editingMessageId().isEmpty());
        QCOMPARE(controller.composerText(), QStringLiteral("half a thought"));

        // Cancelling changes nothing and also gives the draft back.
        QVERIFY(controller.beginEdit(mine));
        controller.setComposerText(QStringLiteral("never mind"));
        controller.cancelComposeMode();
        QCOMPARE(role(1, MessageListModel::BodyRole).toString(),
                 QStringLiteral("Hey Michael, how are you?"));
        QCOMPARE(controller.composerText(), QStringLiteral("half a thought"));

        // A reply names who is answered and quotes them.
        QVERIFY(controller.beginReply(theirs));
        QCOMPARE(controller.replyingToMessageId(), theirs);
        QCOMPARE(controller.composeTargetName(), QStringLiteral("Michael"));
        QCOMPARE(controller.composeTargetText(), QStringLiteral("Hey Daniel!"));
        controller.setComposerText(QStringLiteral("Hi!"));
        QVERIFY(controller.sendMessage());
        const int reply = messages->rowCount() - 1;
        QCOMPARE(role(reply, MessageListModel::BodyRole).toString(), QStringLiteral("Hi!"));
        QCOMPARE(role(reply, MessageListModel::ReplyToIdRole).toString(), theirs);
        QCOMPARE(role(reply, MessageListModel::QuotedSenderRole).toString(),
                 QStringLiteral("Michael"));
        QCOMPARE(role(reply, MessageListModel::QuotedBodyRole).toString(),
                 QStringLiteral("Hey Daniel!"));
        QVERIFY(controller.replyingToMessageId().isEmpty());
        QVERIFY(modeSpy.count() >= 5);

        // Replying to oneself says so; leaving the chat drops the reply.
        QVERIFY(controller.beginReply(mine));
        QCOMPARE(controller.composeTargetName(), QStringLiteral("You"));
        QVERIFY(controller.selectContact(QStringLiteral("sarah")));
        QVERIFY(controller.replyingToMessageId().isEmpty());
        QVERIFY(controller.selectContact(QStringLiteral("michael")));

        // Copy takes the whole text.
        QVERIFY(controller.copyMessage(theirs));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Hey Daniel!"));
        QVERIFY(!controller.copyMessage(QStringLiteral("no such message")));
    }

    void liveEditsAndRepliesTravelBothWays()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        MessageListModel *messages = controller.messages();
        const auto role = [messages](int row, int role) {
            return messages->data(messages->index(row), role);
        };
        const auto peerReads = [&](qsizetype sent) {
            auto processed = live.peer->process(live.conversation,
                                                live.transport->sent.at(sent).ciphertext);
            return processed.hasValue()
                ? decodeMessageContent(processed.value().applicationData) : std::nullopt;
        };

        controller.setComposerText(QStringLiteral("See you at 7"));
        QVERIFY(controller.sendMessage());
        const QString mine = role(0, MessageListModel::StableIdRole).toString();
        // Filed under the id the peer derives from the same ciphertext.
        QCOMPARE(mine, messageIdForCiphertext(live.transport->sent.at(0).ciphertext).toHex());
        QCOMPARE(peerReads(0)->body, QStringLiteral("See you at 7"));
        // Not editable until the relay has it, so an edit cannot overtake it.
        QVERIFY(!role(0, MessageListModel::EditableRole).toBool());
        QVERIFY(!controller.beginEdit(mine));
        live.transport->onRelayAccepted(live.transport->sent.at(0).envelopeId, 7);
        QVERIFY(role(0, MessageListModel::EditableRole).toBool());

        // Our edit: changed here once durable, and the peer is told.
        QVERIFY(controller.beginEdit(mine));
        controller.setComposerText(QStringLiteral("See you at 8"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(role(0, MessageListModel::BodyRole).toString(), QStringLiteral("See you at 8"));
        QVERIFY(role(0, MessageListModel::EditedRole).toBool());
        QCOMPARE(live.transport->sent.size(), qsizetype(2));
        const auto edit = peerReads(1);
        QVERIFY(edit.has_value());
        QCOMPARE(edit->type, MessageContent::Type::Edit);
        QCOMPARE(edit->target->toHex(), mine);
        QCOMPARE(edit->body, QStringLiteral("See you at 8"));

        // The peer writes and then corrects themselves.
        QVERIFY(live.deliverFromPeer(QStringLiteral("hi back")));
        const QString theirs = role(1, MessageListModel::StableIdRole).toString();
        QVERIFY(!role(1, MessageListModel::EditedRole).toBool());
        const auto theirId = MessageId::fromBytes(QByteArray::fromHex(theirs.toLatin1()));
        QVERIFY(theirId);
        QVERIFY(live.deliverFromPeer(
            EnvelopeMessageKind::MlsPrivateMessage,
            encodeMessageContent(MessageContent::edit(*theirId, QStringLiteral("hi back!")))));
        QCOMPARE(messages->rowCount(), 2);
        QCOMPARE(role(1, MessageListModel::BodyRole).toString(), QStringLiteral("hi back!"));
        QVERIFY(role(1, MessageListModel::EditedRole).toBool());
        QVERIFY(!role(1, MessageListModel::EditableRole).toBool());

        // Our reply carries the quote to the peer.
        QVERIFY(controller.beginReply(theirs));
        QCOMPARE(controller.composeTargetName(), QStringLiteral("bob"));
        controller.setComposerText(QStringLiteral("great"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(role(2, MessageListModel::ReplyToIdRole).toString(), theirs);
        QCOMPARE(role(2, MessageListModel::QuotedSenderRole).toString(), QStringLiteral("bob"));
        QCOMPARE(role(2, MessageListModel::QuotedBodyRole).toString(), QStringLiteral("hi back!"));
        const auto reply = peerReads(2);
        QVERIFY(reply.has_value());
        QCOMPARE(reply->type, MessageContent::Type::Reply);
        QCOMPARE(reply->target->toHex(), theirs);
        QCOMPARE(*reply->quotedSender, live.peerDevice);

        // The peer's reply to us names us.
        const DeviceId self = live.session->publicCredential().value().deviceId;
        QVERIFY(live.deliverFromPeer(
            EnvelopeMessageKind::MlsPrivateMessage,
            encodeMessageContent(MessageContent::reply(QStringLiteral("8 works"),
                                                       *MessageId::fromBytes(QByteArray::fromHex(mine.toLatin1())),
                                                       self, QStringLiteral("See you at 8")))));
        QCOMPARE(role(3, MessageListModel::BodyRole).toString(), QStringLiteral("8 works"));
        QCOMPARE(role(3, MessageListModel::QuotedSenderRole).toString(), QStringLiteral("You"));
        QCOMPARE(role(3, MessageListModel::ReplyToIdRole).toString(), mine);

        // All of it is durable. (Rows sent within the same millisecond come
        // back in id order, so each is looked up by its text.)
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        MessageListModel *again = reloaded.messages();
        QCOMPARE(again->rowCount(), 4);
        const auto reread = [again](const QString &body, int role) {
            for (int row = 0; row < again->rowCount(); ++row) {
                if (again->data(again->index(row), MessageListModel::BodyRole).toString() == body)
                    return again->data(again->index(row), role);
            }
            return QVariant();
        };
        QVERIFY(reread(QStringLiteral("See you at 8"), MessageListModel::EditedRole).toBool());
        QVERIFY(reread(QStringLiteral("hi back!"), MessageListModel::EditedRole).toBool());
        QCOMPARE(reread(QStringLiteral("great"), MessageListModel::QuotedSenderRole).toString(),
                 QStringLiteral("bob"));
        QCOMPARE(reread(QStringLiteral("8 works"), MessageListModel::QuotedSenderRole).toString(),
                 QStringLiteral("You"));
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

    void groupStopsClaimingWhenContactBlockedDuringInvite()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ExtraPeer carol;
        QVERIFY(carol.setUp(live, QStringLiteral("carol")));
        int claims = 0;
        std::function<void(const QByteArray &)> pending;
        GroupService groups(*live.session, *live.session->syncEngine(),
            [&](const DeviceId &, std::function<void(const QByteArray &)> done) {
                ++claims;
                pending = std::move(done);
            });
        QSignalSpy failures(&groups, &GroupService::groupActionFailed);
        QSignalSpy created(&groups, &GroupService::groupCreated);
        groups.createGroup({live.peerAccount, carol.account}, QStringLiteral("test"));
        QCOMPARE(claims, 1);
        QVERIFY(pending);
        QVERIFY(live.session->contacts()->block(carol.account, QDateTime::currentMSecsSinceEpoch()).hasValue());
        pending(live.peer->generateKeyPackage().value());
        QCOMPARE(failures.count(), 1);
        QCOMPARE(created.count(), 0);
        QCOMPARE(claims, 1); // Carol's package was never consumed.
        groups.createGroup({carol.account}, QStringLiteral("blocked"));
        QCOMPARE(failures.count(), 2);
        QCOMPARE(claims, 1);
    }

    void groupIsCreatedFromContactsMessagedRenamedAndLeft()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ExtraPeer carol;
        QVERIFY(carol.setUp(live, QStringLiteral("carol")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        // KeyPackages come straight from the peers' own MLS clients, standing in
        // for the relay claim.
        GroupService groups(*live.session, *live.session->syncEngine(),
                            [&](const DeviceId &device, std::function<void(const QByteArray &)> done) {
                                if (device == live.peerDevice)
                                    done(live.peer->generateKeyPackage().value());
                                else if (device == carol.device)
                                    done(carol.mls->generateKeyPackage().value());
                                else
                                    done({});
                            });
        QSignalSpy failures(&groups, &GroupService::groupActionFailed);

        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests, &groups);
        QCOMPARE(controller.contacts()->rowCount(), 2);
        QVERIFY(controller.selectContact(live.peerAccount.toHex()));
        QVERIFY(!controller.currentIsGroup());
        // From Bob's chat the "+" offers everyone else: Carol.
        const QVariantList candidates = controller.groupCandidates();
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().toMap().value(QStringLiteral("contactId")).toString(),
                 carol.account.toHex());

        // Starting a group with Carol makes and opens it.
        controller.addToGroup(carol.account.toHex());
        QCOMPARE(failures.count(), 0);
        QVERIFY(controller.currentIsGroup());
        QCOMPARE(controller.contacts()->rowCount(), 3);
        QCOMPARE(controller.currentGroupMemberCount(), 3);
        QCOMPARE(controller.currentGroupMembers(), QStringLiteral("You, bob, carol"));
        QVERIFY(controller.currentGroupTitle().isEmpty());
        QCOMPARE(controller.currentContactName(), QStringLiteral("bob, carol")); // untitled
        QCOMPARE(controller.currentStatusText(), QStringLiteral("You, bob, carol"));
        QCOMPARE(controller.currentAvatarKey(), QStringLiteral("group"));
        QVERIFY(controller.groupCandidates().isEmpty()); // nobody left to add
        const QString groupId = controller.currentContactId();
        QVERIFY(ChatController::isGroupChatId(groupId));
        const auto route = controller.groupCallRouteFor(groupId);
        QVERIFY(route.has_value());
        QCOMPARE(route->members.size(), 2);

        // Each member got the Welcome and then the roster; both join and read it.
        const auto bobWelcome = sentTo(live, live.peerDevice, EnvelopeMessageKind::GroupWelcome);
        const auto carolWelcome = sentTo(live, carol.device, EnvelopeMessageKind::GroupWelcome);
        QCOMPARE(bobWelcome.size(), 1);
        QCOMPARE(carolWelcome.size(), 1);
        const ConversationId groupConversation = bobWelcome.first().conversationId;
        QCOMPARE(route->conversation, groupConversation);
        QVERIFY(live.peer->joinGroup(groupConversation, bobWelcome.first().ciphertext).hasValue());
        QVERIFY(carol.mls->joinGroup(groupConversation, carolWelcome.first().ciphertext).hasValue());
        const auto bobInfo = sentTo(live, live.peerDevice, EnvelopeMessageKind::GroupControl);
        QCOMPARE(bobInfo.size(), 1);
        auto opened = live.peer->process(groupConversation, bobInfo.first().ciphertext);
        QVERIFY(opened.hasValue());
        const auto info = decodeGroupUpdate(opened.value().applicationData);
        QVERIFY(info.has_value());
        QCOMPARE(info->type, GroupUpdateType::Info);
        QCOMPARE(info->members.size(), 3);
        // Carol reads the same roster from her own copy.
        QVERIFY(carol.mls->process(groupConversation,
                                   sentTo(live, carol.device, EnvelopeMessageKind::GroupControl)
                                       .first()
                                       .ciphertext)
                    .hasValue());
        // The Welcome was queued before the roster, so the join always
        // precedes the message encrypted under the joined epoch.
        int welcomeAt = -1;
        int infoAt = -1;
        for (int i = 0; i < live.transport->sent.size(); ++i) {
            const auto &envelope = live.transport->sent.at(i);
            if (envelope.recipientDeviceId != live.peerDevice)
                continue;
            if (envelope.messageKind == EnvelopeMessageKind::GroupWelcome)
                welcomeAt = i;
            if (envelope.messageKind == EnvelopeMessageKind::GroupControl && infoAt < 0)
                infoAt = i;
        }
        QVERIFY(welcomeAt >= 0 && infoAt > welcomeAt);

        // A message into the group: one row here, one envelope per member, one
        // ciphertext both can read.
        controller.setComposerText(QStringLiteral("hi group"));
        QVERIFY(controller.canSend());
        QVERIFY(controller.sendMessage());
        QCOMPARE(controller.messages()->rowCount(), 4);
        const auto toBob = sentTo(live, live.peerDevice, EnvelopeMessageKind::MlsPrivateMessage);
        const auto toCarol = sentTo(live, carol.device, EnvelopeMessageKind::MlsPrivateMessage);
        QCOMPARE(toBob.size(), 1);
        QCOMPARE(toCarol.size(), 1);
        QCOMPARE(toBob.first().ciphertext, toCarol.first().ciphertext);
        QCOMPARE(live.peer->process(groupConversation, toBob.first().ciphertext)
                     .value()
                     .applicationData,
                 QByteArray("hi group"));
        QCOMPARE(carol.mls->process(groupConversation, toCarol.first().ciphertext)
                     .value()
                     .applicationData,
                 QByteArray("hi group"));

        // A member's message lands in the open group, named after its sender.
        auto fromCarol = carol.mls->encrypt(groupConversation, QByteArray("carol here"));
        QVERIFY(fromCarol.hasValue());
        controller.setConversationVisible(false);
        deliver(live, carol.account, carol.device, groupConversation,
                EnvelopeMessageKind::MlsPrivateMessage, fromCarol.value().bytes);
        QCOMPARE(controller.contacts()->contactById(groupId)->unreadCount, 1);
        QCOMPARE(controller.contacts()->contactAt(0)->id, groupId);
        controller.setConversationVisible(true);
        QCOMPARE(controller.contacts()->contactById(groupId)->unreadCount, 0);
        QCOMPARE(controller.messages()->rowCount(), 5);
        const QModelIndex inRow = controller.messages()->index(4);
        QCOMPARE(controller.messages()->data(inRow, MessageListModel::BodyRole).toString(),
                 QStringLiteral("carol here"));
        QCOMPARE(controller.messages()->data(inRow, MessageListModel::SenderNameRole).toString(),
                 QStringLiteral("carol"));
        // Our own row carries no sender name; a one-to-one message neither.
        QVERIFY(controller.messages()
                    ->data(controller.messages()->index(3), MessageListModel::SenderNameRole)
                    .toString()
                    .isEmpty());

        // Renaming, the way the status line is edited, reaches every member.
        QVERIFY(controller.renameCurrentGroup(QStringLiteral("  Weekend plans  ")));
        QCOMPARE(controller.currentGroupTitle(), QStringLiteral("Weekend plans"));
        QCOMPARE(controller.currentContactName(), QStringLiteral("Weekend plans"));
        const auto bobControls = sentTo(live, live.peerDevice, EnvelopeMessageKind::GroupControl);
        QCOMPARE(bobControls.size(), 2);
        auto renamed = decodeGroupUpdate(
            live.peer->process(groupConversation, bobControls.at(1).ciphertext).value().applicationData);
        QVERIFY(renamed.has_value());
        QCOMPARE(renamed->type, GroupUpdateType::Rename);
        QCOMPARE(renamed->title, QStringLiteral("Weekend plans"));
        // It is durable: a fresh controller shows the title and the history.
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests, &groups);
        QVERIFY(reloaded.selectContact(groupId));
        QCOMPARE(reloaded.currentGroupTitle(), QStringLiteral("Weekend plans"));
        QCOMPARE(reloaded.messages()->rowCount(), 5);

        // Group call fan-out creates one outgoing history row, regardless of
        // how many members receive an offer.
        const auto callOffer = encodeCallSignal(CallSignalMessage::offer(
            CallId::generate(), generateCallSecret(), AudioCodecKind::Pcm));
        live.session->syncEngine()->sendCallSignal(groupConversation, live.peerDevice, callOffer);
        live.session->syncEngine()->sendCallSignal(groupConversation, carol.device, callOffer);
        QCOMPARE(controller.messages()->rowCount(), 6);
        QCOMPARE(controller.messages()->messageAt(5)->kind, MessageKind::CallEvent);
        QCOMPARE(controller.messages()->messageAt(5)->direction, MessageDirection::Outgoing);
        QVERIFY(controller.messages()->messageAt(5)->body.endsWith(QStringLiteral(" Started a group call")));

        // Leaving tells everyone and drops the chat here; the view falls back
        // to a person.
        controller.leaveCurrentGroup();
        QVERIFY(!controller.currentIsGroup());
        QCOMPARE(controller.contacts()->rowCount(), 2);
        QVERIFY(!controller.groupCallRouteFor(groupId).has_value());
        const auto afterLeave = sentTo(live, live.peerDevice, EnvelopeMessageKind::GroupControl);
        QCOMPARE(afterLeave.size(), 3);
        auto left = decodeGroupUpdate(
            live.peer->process(groupConversation, afterLeave.at(2).ciphertext).value().applicationData);
        QVERIFY(left.has_value());
        QCOMPARE(left->type, GroupUpdateType::Leave);
        QCOMPARE(failures.count(), 0);
        QVERIFY(!live.session->syncEngine()->isFailedClosed());
    }

    void inboundGroupWelcomeOpensTheChatAndMembersComeAndGo()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        GroupService groups(*live.session, *live.session->syncEngine(), {});
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests, &groups);
        QCOMPARE(controller.contacts()->rowCount(), 1);

        // Bob makes a group with us and a stranger (not our contact), from a
        // KeyPackage we published.
        auto ourKeyPackage = live.session->mls()->generateKeyPackage();
        QVERIFY(ourKeyPackage.hasValue());
        QVERIFY(live.session->persistMlsState().hasValue());
        auto stranger = std::move(MlsClient::create(credentialFor(DeviceId::generate()))).value();
        const DeviceId strangerDevice = DeviceId::generate();
        const AccountId strangerAccount = AccountId::generate();
        auto strangerClient = std::move(MlsClient::create(credentialFor(strangerDevice))).value();
        const ConversationId group = ConversationId::generate();
        QVERIFY(live.peer->createGroup(group).hasValue());
        auto added = live.peer->addMembers(
            group, {ourKeyPackage.value(), strangerClient->generateKeyPackage().value()});
        QVERIFY(added.hasValue());
        QVERIFY(strangerClient->joinGroup(group, added.value().welcome).hasValue());

        // The Welcome from an accepted contact joins us straight away, and the
        // chat appears with the one member we can already name.
        QSignalSpy joined(&groups, &GroupService::groupJoined);
        deliver(live, live.peerAccount, live.peerDevice, group, EnvelopeMessageKind::GroupWelcome,
                added.value().welcome);
        QCOMPARE(joined.count(), 1);
        QCOMPARE(controller.contacts()->rowCount(), 2);
        const QString groupId = controller.groupChatIdFor(group);
        QVERIFY(!groupId.isEmpty());
        QVERIFY(controller.selectContact(groupId));
        QCOMPARE(controller.currentGroupMembers(), QStringLiteral("You, bob"));

        // The roster follows: title, and the stranger under the name Bob gave.
        const auto ourDevice = live.session->publicCredential().value().deviceId;
        const auto ourAccount = live.session->accountId().value();
        const auto info = GroupUpdateMessage::info(
            QStringLiteral("Lunch"),
            {GroupMemberInfo{live.peerAccount, live.peerDevice, QStringLiteral("Robert")},
             GroupMemberInfo{ourAccount, ourDevice, QStringLiteral("me")},
             GroupMemberInfo{strangerAccount, strangerDevice, QStringLiteral("dana")}});
        deliver(live, live.peerAccount, live.peerDevice, group, EnvelopeMessageKind::GroupControl,
                live.peer->encrypt(group, encodeGroupUpdate(info)).value().bytes);
        QCOMPARE(controller.currentGroupTitle(), QStringLiteral("Lunch"));
        QCOMPARE(controller.currentContactName(), QStringLiteral("Lunch"));
        // Bob keeps our roster's name, not the one he sent for himself.
        QCOMPARE(controller.currentGroupMembers(), QStringLiteral("You, bob, dana"));
        QCOMPARE(controller.currentGroupMemberCount(), 3);
        // The stranger is not a contact, so cannot be offered by the "+".
        QVERIFY(controller.groupCandidates().isEmpty());

        QCOMPARE(controller.messages()->rowCount(), 3);
        // Refreshing an unchanged roster must not announce everyone joining again.
        deliver(live, live.peerAccount, live.peerDevice, group, EnvelopeMessageKind::GroupControl,
                live.peer->encrypt(group, encodeGroupUpdate(info)).value().bytes);
        QCOMPARE(controller.messages()->rowCount(), 3);

        // A message from the stranger is readable and named.
        deliver(live, strangerAccount, strangerDevice, group, EnvelopeMessageKind::MlsPrivateMessage,
                strangerClient->encrypt(group, QByteArray("hey")).value().bytes);
        QCOMPARE(controller.messages()->rowCount(), 4);
        QCOMPARE(controller.messages()
                     ->data(controller.messages()->index(3), MessageListModel::SenderNameRole)
                     .toString(),
                 QStringLiteral("dana"));

        // Someone renames it.
        deliver(live, strangerAccount, strangerDevice, group, EnvelopeMessageKind::GroupControl,
                strangerClient->encrypt(group, encodeGroupUpdate(GroupUpdateMessage::rename(
                                                   QStringLiteral("Late lunch"))))
                    .value()
                    .bytes);
        QCOMPARE(controller.currentGroupTitle(), QStringLiteral("Late lunch"));

        // The stranger leaves: gone from the roster, and whoever has the lowest
        // device id re-keys the group without them.
        deliver(live, strangerAccount, strangerDevice, group, EnvelopeMessageKind::GroupControl,
                strangerClient->encrypt(group, encodeGroupUpdate(GroupUpdateMessage::leave()))
                    .value()
                    .bytes);
        QCOMPARE(controller.currentGroupMembers(), QStringLiteral("You, bob"));
        QCOMPARE(controller.messages()->rowCount(), 5);
        QCOMPARE(controller.messages()->messageAt(4)->body, QStringLiteral("dana Left the group chat"));
        QCOMPARE(controller.messages()->messageAt(4)->kind, MessageKind::MembershipEvent);
        QCoreApplication::processEvents();
        const bool weCommit = ourDevice.bytes() < live.peerDevice.bytes();
        const auto commits = sentTo(live, live.peerDevice, EnvelopeMessageKind::MlsCommit);
        QCOMPARE(commits.size(), weCommit ? 1 : 0);
        if (weCommit) {
            // Bob applies our removal commit; the stranger can no longer read.
            auto applied = live.peer->process(group, commits.first().ciphertext);
            QVERIFY(applied.hasValue());
            QCOMPARE(applied.value().kind, MlsProcessKind::Commit);
            QCOMPARE(live.peer->groupMembers(group).value().size(), qsizetype(1));
            auto afterwards = live.peer->encrypt(group, QByteArray("just us"));
            QVERIFY(!strangerClient->process(group, afterwards.value().bytes).hasValue());
            deliver(live, live.peerAccount, live.peerDevice, group,
                    EnvelopeMessageKind::MlsPrivateMessage, afterwards.value().bytes);
            QCOMPARE(controller.messages()->rowCount(), 6);
        }
        QVERIFY(!live.session->syncEngine()->isFailedClosed());
        (void)stranger;
    }

    void inboundGroupWelcomeFromAStrangerIsIgnored()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        GroupService groups(*live.session, *live.session->syncEngine(), {});
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests, &groups);

        // Bob is NOT an accepted contact here: his Welcome is consumed unjoined.
        auto ourKeyPackage = live.session->mls()->generateKeyPackage();
        QVERIFY(ourKeyPackage.hasValue());
        QVERIFY(live.session->persistMlsState().hasValue());
        const ConversationId group = ConversationId::generate();
        QVERIFY(live.peer->createGroup(group).hasValue());
        auto added = live.peer->addMembers(group, {ourKeyPackage.value()});
        QVERIFY(added.hasValue());
        QSignalSpy joined(&groups, &GroupService::groupJoined);
        deliver(live, live.peerAccount, live.peerDevice, group, EnvelopeMessageKind::GroupWelcome,
                added.value().welcome);
        QCOMPARE(joined.count(), 0);
        QCOMPARE(controller.contacts()->rowCount(), 0);
        QVERIFY(controller.groupChatIdFor(group).isEmpty());
        QVERIFY(!live.session->syncEngine()->isFailedClosed());
    }

    void mockGroupsWorkWithoutServices()
    {
        ChatController controller;
        QCOMPARE(controller.currentContactName(), QStringLiteral("Michael"));
        QVERIFY(!controller.currentIsGroup());
        // Everyone but Michael can be added.
        QCOMPARE(controller.groupCandidates().size(), 5);
        QSignalSpy changed(&controller, &ChatController::currentContactChanged);

        controller.addToGroup(QStringLiteral("sarah"));
        QVERIFY(controller.currentIsGroup());
        QCOMPARE(controller.currentContactName(), QStringLiteral("Michael, Sarah"));
        QCOMPARE(controller.currentGroupMembers(), QStringLiteral("You, Michael, Sarah"));
        QCOMPARE(controller.currentAvatarKey(), QStringLiteral("group"));
        QCOMPARE(controller.contacts()->rowCount(), 7);
        QCOMPARE(controller.groupCandidates().size(), 4);
        QVERIFY(changed.count() >= 1);
        const QString groupId = controller.currentContactId();

        // Adding to the open group grows it; adding a member twice is a no-op.
        controller.addToGroup(QStringLiteral("alex"));
        controller.addToGroup(QStringLiteral("alex"));
        QCOMPARE(controller.currentContactId(), groupId);
        QCOMPARE(controller.currentGroupMemberCount(), 4);
        QCOMPARE(controller.groupCandidates().size(), 3);

        // Rename, and the row follows; leave, and the chat is gone.
        QVERIFY(controller.renameCurrentGroup(QStringLiteral("Weekend plans")));
        QCOMPARE(controller.currentContactName(), QStringLiteral("Weekend plans"));
        QCOMPARE(controller.contacts()->contactById(groupId)->name, QStringLiteral("Weekend plans"));
        controller.setComposerText(QStringLiteral("hello group"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(controller.messages()->rowCount(), 1);
        controller.leaveCurrentGroup();
        QVERIFY(!controller.currentIsGroup());
        QCOMPARE(controller.contacts()->rowCount(), 6);
        QVERIFY(!controller.contacts()->contactById(groupId).has_value());
        // Renaming with no group open is refused.
        QVERIFY(!controller.renameCurrentGroup(QStringLiteral("x")));
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
        // The call belongs to the caller's chat, which is the one open; a
        // caller with no chat at all is shown wherever the user is.
        QCOMPARE(calls.callChatId(), knownConversation ? live.peerAccount.toHex() : QString());
        QVERIFY(calls.callInCurrentChat());
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

    // A call tile opens its member's profile by account, so every member of a
    // group route carries one, including someone who is not a contact here
    // (they have no roster id), and so does a one-to-one call.
    void groupCallRoutesCarryAccountIds()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        GroupService groups(*live.session, *live.session->syncEngine(), {});
        ChatController chats;
        chats.setLiveServices(live.session.get(), live.session->syncEngine(), &requests, &groups);

        // Bob puts us in a group with Dana, who is nobody's contact here.
        auto ourKeyPackage = live.session->mls()->generateKeyPackage();
        QVERIFY(ourKeyPackage.hasValue());
        QVERIFY(live.session->persistMlsState().hasValue());
        const DeviceId danaDevice = DeviceId::generate();
        const AccountId danaAccount = AccountId::generate();
        auto dana = std::move(MlsClient::create(credentialFor(danaDevice))).value();
        const ConversationId group = ConversationId::generate();
        QVERIFY(live.peer->createGroup(group).hasValue());
        auto added = live.peer->addMembers(
            group, {ourKeyPackage.value(), dana->generateKeyPackage().value()});
        QVERIFY(added.hasValue());
        deliver(live, live.peerAccount, live.peerDevice, group, EnvelopeMessageKind::GroupWelcome,
                added.value().welcome);
        const DeviceId ourDevice = live.session->publicCredential().value().deviceId;
        const auto info = GroupUpdateMessage::info(
            QStringLiteral("Lunch"),
            {GroupMemberInfo{live.peerAccount, live.peerDevice, QStringLiteral("Robert")},
             GroupMemberInfo{live.session->accountId().value(), ourDevice, QStringLiteral("me")},
             GroupMemberInfo{danaAccount, danaDevice, QStringLiteral("dana")}});
        deliver(live, live.peerAccount, live.peerDevice, group, EnvelopeMessageKind::GroupControl,
                live.peer->encrypt(group, encodeGroupUpdate(info)).value().bytes);
        const QString groupId = chats.groupChatIdFor(group);
        QVERIFY(!groupId.isEmpty());

        const auto route = chats.groupCallRouteFor(groupId);
        QVERIFY(route.has_value());
        QCOMPARE(route->members.size(), 2);
        QHash<QString, CallEngine::CallPeer> members;
        for (const CallEngine::CallPeer &peer : route->members)
            members.insert(peer.device.toHex(), peer);
        QVERIFY(members.contains(live.peerDevice.toHex()));
        QVERIFY(members.contains(danaDevice.toHex()));
        const CallEngine::CallPeer bob = members.value(live.peerDevice.toHex());
        QCOMPARE(bob.accountId, live.peerAccount.toHex());
        QCOMPARE(bob.contactId, live.peerAccount.toHex());
        const CallEngine::CallPeer stranger = members.value(danaDevice.toHex());
        QCOMPARE(stranger.accountId, danaAccount.toHex());
        QVERIFY(stranger.contactId.isEmpty());
        QCOMPARE(stranger.displayName, QStringLiteral("dana"));

        // Ringing the group puts each account on its participant row, what a
        // group call tile hands to the profile.
        SyncCallTransport transport(*live.session->syncEngine());
        CallTest::ScriptedAudioDevices devices;
        CallEngine::Config config;
        config.localDevice = ourDevice;
        CallEngine engine(config, transport, devices.factory());
        CallController calls;
        calls.setLiveEngine(&engine, &chats);
        QVERIFY(chats.selectContact(groupId));
        calls.callCurrentContact();
        QCOMPARE(engine.state(), CallState::Dialing);
        QVERIFY(calls.isGroupCall());
        CallParticipantModel *rows = calls.participants();
        QCOMPARE(rows->rowCount(), 2);
        QHash<QString, QString> accountOf;
        for (int row = 0; row < rows->rowCount(); ++row) {
            const QModelIndex index = rows->index(row);
            accountOf.insert(rows->data(index, CallParticipantModel::DeviceIdRole).toString(),
                             rows->data(index, CallParticipantModel::AccountIdRole).toString());
        }
        QCOMPARE(accountOf.value(live.peerDevice.toHex()), live.peerAccount.toHex());
        QCOMPARE(accountOf.value(danaDevice.toHex()), danaAccount.toHex());

        // A one-to-one call to a contact carries their account too.
        calls.hangUp();
        calls.dismissCall();
        QCOMPARE(engine.state(), CallState::Idle);
        QVERIFY(chats.selectContact(live.peerAccount.toHex()));
        calls.callCurrentContact();
        QCOMPARE(engine.state(), CallState::Dialing);
        QVERIFY(!calls.isGroupCall());
        QCOMPARE(engine.peer().accountId, live.peerAccount.toHex());
        QCOMPARE(calls.callChatId(), live.peerAccount.toHex());
        calls.hangUp();
        QVERIFY(!live.session->syncEngine()->isFailedClosed());
    }

    // The previews' one-to-one far-end tile opens the chat the call belongs
    // to; a live controller takes it from the engine and ignores the seam.
    void previewCallChatIdIsForPreviewsOnly()
    {
        using namespace OpenChat;
        CallController preview;
        QVERIFY(preview.callChatId().isEmpty());
        QSignalSpy changed(&preview, &CallController::callChanged);
        preview.setPreviewCallChatId(QStringLiteral("jessica"));
        QCOMPARE(preview.callChatId(), QStringLiteral("jessica"));
        QCOMPARE(changed.count(), 1);
        preview.setPreviewCallChatId(QStringLiteral("jessica"));
        QCOMPARE(changed.count(), 1);
        preview.setPreviewCallChatId(QString());
        QVERIFY(preview.callChatId().isEmpty());
        QCOMPARE(changed.count(), 2);

        LiveFixture live;
        QVERIFY(live.setUp());
        ChatController chats;
        SyncCallTransport transport(*live.session->syncEngine());
        CallTest::ScriptedAudioDevices devices;
        CallEngine engine(CallEngine::Config{}, transport, devices.factory());
        CallController calls;
        calls.setLiveEngine(&engine, &chats);
        calls.setPreviewCallChatId(QStringLiteral("jessica"));
        QVERIFY(calls.callChatId().isEmpty());
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

        // Appearing offline reads Offline even with words set, the way
        // contacts see it; the words come back with the presence.
        controller.setLocalStatusText(QStringLiteral("Not available"));
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Offline"));
        controller.setLocalPresence(static_cast<int>(OpenChat::Presence::Busy));
        QCOMPARE(controller.localStatusLine(), QStringLiteral("Not available"));
        controller.setLocalStatusText(QString());
        controller.setLocalPresence(2);

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

    void chatControllerOwnsAProfileController()
    {
        ChatController controller;
        OpenChat::ProfileController *profiles = controller.profiles();
        QVERIFY(profiles != nullptr);
        QCOMPARE(controller.profiles(), profiles);
        // QML reads it as a constant property: every avatar site in the
        // window opens profiles through this one instance.
        const QMetaObject *meta = controller.metaObject();
        const QMetaProperty property = meta->property(meta->indexOfProperty("profiles"));
        QVERIFY(property.isValid());
        QVERIFY(property.isConstant());
        QVERIFY(!property.isWritable());
        QCOMPARE(controller.property("profiles").value<OpenChat::ProfileController *>(), profiles);
        // It is not a QObject child: the controller's own member order, not
        // QObject's child teardown, decides when it goes.
        QVERIFY(!controller.children().contains(profiles));
        // It runs the reference mock until live services arrive.
        QVERIFY(profiles->sync() == nullptr);
        QVERIFY(profiles->openContact(QStringLiteral("michael")));
        QCOMPARE(profiles->personName(), controller.contacts()->contactById(QStringLiteral("michael"))->name);
    }

    void profileControllerDiesBeforeTheRoster()
    {
        // Run under MALLOC_PERTURB_: the profile controller reads the roster
        // and the local profile to its very end (its destructor saves the
        // editor's draft), so it must be destroyed before them.
        auto controller = std::make_unique<ChatController>();
        OpenChat::ProfileController *profiles = controller->profiles();
        QStringList order;
        connect(profiles, &QObject::destroyed, this, [&order] { order.append(QStringLiteral("profiles")); });
        connect(controller->contacts(), &QObject::destroyed, this,
                [&order] { order.append(QStringLiteral("roster")); });
        connect(controller->messages(), &QObject::destroyed, this,
                [&order] { order.append(QStringLiteral("messages")); });
        // Leave it mid-edit, with a Top Friend tile resolved from the roster.
        profiles->openOwn();
        QVERIFY(profiles->beginEditing());
        QVERIFY(profiles->addTopFriend(QStringLiteral("jessica")));
        profiles->draft()->setHeadline(QStringLiteral("unsaved"));
        controller.reset();
        QCOMPARE(order.value(0), QStringLiteral("profiles"));
        QVERIFY(order.contains(QStringLiteral("roster")));
        QVERIFY(order.contains(QStringLiteral("messages")));
    }

    void liveProfilesPublishThroughTheChatController()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        ProfileController &profiles = *controller.profiles();
        QVERIFY(profiles.sync() != nullptr);

        // Nothing is sent for pages until one is published.
        QTest::qWait(50);
        QCOMPARE(countProfileUpdates(live), 0);

        profiles.openOwn();
        QVERIFY(profiles.beginEditing());
        profiles.draft()->setHeadline(QStringLiteral("Hello from my page"));
        QVERIFY(profiles.publish());
        const qint64 revision = profiles.publishedRevision();
        QVERIFY(revision > 0);
        QTRY_COMPARE(countProfileUpdates(live), 1);
        // Published once, delivered once: nothing more follows.
        QTest::qWait(100);
        QCOMPARE(countProfileUpdates(live), 1);

        const CiphertextEnvelopeV1 &envelope = live.transport->sent.last();
        QCOMPARE(envelope.messageKind, EnvelopeMessageKind::ProfileUpdate);
        QCOMPARE(envelope.recipientDeviceId.bytes(), live.peerDevice.bytes());
        QCOMPARE(envelope.conversationId.bytes(), live.conversation.bytes());
        const QVector<QByteArray> payloads = decryptedProfilePayloads(live);
        QCOMPARE(payloads.size(), 1);
        QCOMPARE(classifyProfilePayload(payloads.first()), ProfilePayloadKind::PageCore);
        const std::optional<Profile::Page> page = decodePageCore(payloads.first());
        QVERIFY(page.has_value());
        QCOMPARE(page->revision, revision);
        QCOMPARE(page->content.headline, QStringLiteral("Hello from my page"));
        // A 0.2.8 client's decoder ignores it: it is not a legacy profile.
        QVERIFY(!decodeProfileUpdate(payloads.first()).has_value());
        QVERIFY(!live.session->syncEngine()->isFailedClosed());

        // A status edit still goes out as the legacy profile, alongside.
        controller.setLocalStatusText(QStringLiteral("Status still works"));
        QCOMPARE(countProfileUpdates(live), 2);
        const QVector<QByteArray> both = decryptedProfilePayloads(live);
        QCOMPARE(both.size(), 2);
        QCOMPARE(both.first(), payloads.first()); // decrypted once, served from the cache
        QCOMPARE(classifyProfilePayload(both.last()), ProfilePayloadKind::Legacy);
        const auto legacy = decodeProfileUpdate(both.last());
        QVERIFY(legacy.has_value());
        QCOMPARE(legacy->statusText, QStringLiteral("Status still works"));
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

        // The row shows their picture. Their chosen presence and words are
        // remembered but only shown while their device is reachable, which
        // without a relay it never is: bead and status line both read Offline.
        QVERIFY(contactSpy.count() >= 1);
        QCOMPARE(controller.currentStatusText(), QStringLiteral("Offline"));
        QCOMPARE(controller.currentAvatarKey(), AvatarStore::keyFor(jpeg));
        QVERIFY(AvatarStore::instance().image(controller.currentAvatarKey()).has_value());
        QCOMPARE(controller.currentPresence(), static_cast<int>(Presence::Offline));
        QCOMPARE(controller.messages()->rowCount(), 0); // not conversation
        const QModelIndex row = controller.contacts()->index(0);
        QCOMPARE(controller.contacts()->data(row, OpenChat::ContactListModel::StatusTextRole)
                     .toString(),
                 QStringLiteral("Offline"));

        // It is durable: the roster row holds it, and a restart shows it again.
        auto stored = live.session->contacts()->find(live.peerAccount);
        QVERIFY(stored.hasValue() && stored.value().has_value());
        QCOMPARE(stored.value()->presence, static_cast<int>(Presence::Busy));
        QCOMPARE(stored.value()->statusText, QStringLiteral("Deep in code"));
        QCOMPARE(stored.value()->avatarJpeg, jpeg);
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QCOMPARE(reloaded.currentStatusText(), QStringLiteral("Offline"));
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

    // --- Attachments (docs/chat-attachments.md)

    void attachmentsArePreparedInTheTrayAndSentAsRows()
    {
        using namespace OpenChat;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString photo = writePhoto(dir, QStringLiteral("beach.png"), Qt::blue);
        const QString notes = writeFile(dir, QStringLiteral("notes.txt"), QByteArray("Bring sunscreen.\n"));
        ChatController controller;
        auto *tray = qobject_cast<StagedAttachmentModel *>(controller.stagedAttachments());
        QVERIFY(tray);
        const auto card = [tray](int row, int role) { return tray->data(tray->index(row), role); };
        QSignalSpy changed(&controller, &ChatController::stagedAttachmentsChanged);

        controller.attachFiles({QUrl::fromLocalFile(photo), QUrl::fromLocalFile(notes)});
        QCOMPARE(tray->rowCount(), 2);
        QVERIFY(controller.hasStagedAttachments());
        QVERIFY(controller.stagingBusy());
        QVERIFY(changed.count() > 0);
        // Nothing typed, yet something to send.
        QVERIFY(controller.canSend());
        QCOMPARE(card(0, StagedAttachmentModel::KindRole).toInt(), int(AttachmentKind::Image));
        QCOMPARE(card(1, StagedAttachmentModel::NameRole).toString(), QStringLiteral("notes.txt"));
        QCOMPARE(card(1, StagedAttachmentModel::SizeTextRole).toString(), QStringLiteral("17 bytes"));

        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        QVERIFY(card(0, StagedAttachmentModel::ReadyRole).toBool());
        QVERIFY(card(1, StagedAttachmentModel::ReadyRole).toBool());
        QCOMPARE(card(0, StagedAttachmentModel::ProgressRole).toReal(), 1.0);
        // The photo is shown by its preview; the name says what will be sent.
        const QString previewKey = card(0, StagedAttachmentModel::PreviewKeyRole).toString();
        QVERIFY(!previewKey.isEmpty());
        QVERIFY(PanelMediaLibrary::instance().contains(previewKey));
        QCOMPARE(card(0, StagedAttachmentModel::NameRole).toString(), QStringLiteral("beach.jpg"));

        MessageListModel *messages = controller.messages();
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };
        const int before = messages->rowCount();
        controller.setComposerText(QStringLiteral("From the trip"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(messages->rowCount(), before + 2);
        // The caption goes with the first; both are ready at once in a preview.
        QCOMPARE(role(before, MessageListModel::KindRole).toInt(), int(MessageKind::Attachment));
        QCOMPARE(role(before, MessageListModel::AttachmentKindRole).toInt(), int(AttachmentKind::Image));
        QCOMPARE(role(before, MessageListModel::BodyRole).toString(), QStringLiteral("From the trip"));
        QCOMPARE(role(before, MessageListModel::MediaWidthRole).toInt(), 120);
        QCOMPARE(role(before, MessageListModel::MediaHeightRole).toInt(), 90);
        QCOMPARE(role(before, MessageListModel::TransferStateRole).toInt(), int(AttachmentTransferState::Ready));
        QVERIFY(role(before, MessageListModel::CanSaveRole).toBool());
        QVERIFY(role(before, MessageListModel::HasPreviewRole).toBool());
        QVERIFY(!role(before, MessageListModel::EditableRole).toBool());
        QCOMPARE(role(before + 1, MessageListModel::AttachmentKindRole).toInt(), int(AttachmentKind::File));
        QCOMPARE(role(before + 1, MessageListModel::BodyRole).toString(), QString());
        QCOMPARE(role(before + 1, MessageListModel::FileNameRole).toString(), QStringLiteral("notes.txt"));
        QCOMPARE(role(before + 1, MessageListModel::MimeTypeRole).toString(), QStringLiteral("text/plain"));
        QCOMPARE(role(before + 1, MessageListModel::ByteCountRole).toDouble(), 17.0);
        QCOMPARE(role(before + 1, MessageListModel::TransferTextRole).toString(), QString());
        // The tray and the composer are empty again, and its keys released.
        QCOMPARE(tray->rowCount(), 0);
        QVERIFY(!controller.hasStagedAttachments());
        QCOMPARE(controller.composerText(), QString());
        QVERIFY(!controller.canSend());
        QVERIFY(!PanelMediaLibrary::instance().contains(previewKey));
    }

    void aSendAskedForEarlyWaitsForTheTray()
    {
        using namespace OpenChat;
        QTemporaryDir dir;
        const QString photo = writePhoto(dir, QStringLiteral("dog.png"), Qt::darkGreen);
        ChatController controller;
        MessageListModel *messages = controller.messages();
        const int before = messages->rowCount();

        controller.attachFiles({QUrl::fromLocalFile(photo)});
        QVERIFY(controller.stagingBusy());
        controller.setComposerText(QStringLiteral("Soon"));
        QVERIFY(controller.canSend());
        QVERIFY(controller.sendMessage());
        QVERIFY(controller.sendWhenReady());
        // The text stays until it goes, with its photo.
        QCOMPARE(controller.composerText(), QStringLiteral("Soon"));
        QCOMPARE(messages->rowCount(), before);
        QTRY_COMPARE_WITH_TIMEOUT(messages->rowCount(), before + 1, 20'000);
        QVERIFY(!controller.sendWhenReady());
        QCOMPARE(controller.composerText(), QString());
        QCOMPARE(messages->data(messages->index(before), MessageListModel::BodyRole).toString(),
                 QStringLiteral("Soon"));

        // Changing the text meanwhile means the message is not finished.
        controller.attachFiles({QUrl::fromLocalFile(photo)});
        controller.setComposerText(QStringLiteral("Wait"));
        QVERIFY(controller.sendMessage());
        QVERIFY(controller.sendWhenReady());
        controller.setComposerText(QStringLiteral("Wait, one more"));
        QVERIFY(!controller.sendWhenReady());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        QTest::qWait(20);
        QCOMPARE(messages->rowCount(), before + 1);
        // So does removing a card, or starting an edit.
        controller.attachFiles({QUrl::fromLocalFile(photo)});
        QVERIFY(controller.sendMessage());
        QVERIFY(controller.sendWhenReady());
        auto *tray = qobject_cast<StagedAttachmentModel *>(controller.stagedAttachments());
        const QString second = tray->data(tray->index(1), StagedAttachmentModel::StagedIdRole).toString();
        controller.removeStagedAttachment(second);
        QVERIFY(!controller.sendWhenReady());
        QCOMPARE(tray->rowCount(), 1);
        controller.clearStagedAttachments();
        QCOMPARE(tray->rowCount(), 0);
        QVERIFY(!controller.hasStagedAttachments());
    }

    void aCaptionNeverGoesOutWithoutItsAttachment()
    {
        using namespace OpenChat;
        QTemporaryDir dir;
        ChatController controller;
        MessageListModel *messages = controller.messages();
        const int before = messages->rowCount();
        auto *tray = qobject_cast<StagedAttachmentModel *>(controller.stagedAttachments());

        // Enter while the card is still being prepared, then it fails: the
        // caption was written for the photo, so it waits in the field.
        const QString empty = writeFile(dir, QStringLiteral("beach.jpg"), QByteArray());
        controller.attachFiles({QUrl::fromLocalFile(empty)});
        QVERIFY(controller.stagingBusy());
        controller.setComposerText(QStringLiteral("The beach on Saturday"));
        QVERIFY(controller.sendMessage());
        QVERIFY(controller.sendWhenReady());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        QTRY_VERIFY(!controller.sendWhenReady());
        QTest::qWait(20);
        QCOMPARE(messages->rowCount(), before);
        QCOMPARE(controller.composerText(), QStringLiteral("The beach on Saturday"));
        QCOMPARE(tray->rowCount(), 1);
        // Sent on purpose now, with the failure in view, it is just a text.
        QVERIFY(controller.sendMessage());
        QCOMPARE(messages->rowCount(), before + 1);
        QCOMPARE(messages->data(messages->index(before), MessageListModel::KindRole).toInt(), int(MessageKind::Text));
    }

    void pastingAttachesAPictureButNeverTakesText()
    {
        using namespace OpenChat;
        ChatController controller;
        auto *tray = qobject_cast<StagedAttachmentModel *>(controller.stagedAttachments());
        QClipboard *clipboard = QGuiApplication::clipboard();
        QImage picture(120, 80, QImage::Format_RGB32);
        picture.fill(Qt::darkCyan);

        // Copied spreadsheet cells: their text, and a picture of them. The
        // text is what was meant, so the paste is left to the field.
        auto *cells = new QMimeData;
        cells->setText(QStringLiteral("Qty\tItem\n3\tTickets"));
        cells->setHtml(QStringLiteral("<table><tr><td>3</td><td>Tickets</td></tr></table>"));
        cells->setImageData(picture);
        clipboard->setMimeData(cells);
        QVERIFY(!controller.attachClipboard());
        QCOMPARE(tray->rowCount(), 0);

        // A screenshot, or "Copy image" in a browser (a picture and markup,
        // no text), is attached.
        auto *copied = new QMimeData;
        copied->setHtml(QStringLiteral("<img src=\"https://example.com/cat.jpg\">"));
        copied->setImageData(picture);
        clipboard->setMimeData(copied);
        QVERIFY(controller.attachClipboard());
        QCOMPARE(tray->rowCount(), 1);
        controller.clearStagedAttachments();

        // Plain text is nothing to attach.
        clipboard->setText(QStringLiteral("just words"));
        QVERIFY(!controller.attachClipboard());
        QCOMPARE(tray->rowCount(), 0);
        clipboard->clear();
    }

    void attachRefusalsSayWhy()
    {
        using namespace OpenChat;
        QTemporaryDir dir;
        ChatController controller;
        auto *tray = qobject_cast<StagedAttachmentModel *>(controller.stagedAttachments());
        QSignalSpy notice(&controller, &ChatController::attachmentNoticeChanged);

        controller.attachFiles({QUrl(QStringLiteral("https://example.com/cat.jpg"))});
        QCOMPARE(controller.attachmentNotice(), QStringLiteral("Only files on this computer can be attached."));
        QCOMPARE(tray->rowCount(), 0);
        controller.attachFiles({QUrl::fromLocalFile(dir.path())});
        QCOMPARE(controller.attachmentNotice(), QStringLiteral("Folders can't be attached."));
        // A file goes as it is: too large, it is refused before any of it is read.
        QFile large(dir.filePath(QStringLiteral("backup.bin")));
        QVERIFY(large.open(QIODevice::WriteOnly));
        QVERIFY(large.resize(AttachmentLimits::maxFileBytes + 1));
        large.close();
        controller.attachFiles({QUrl::fromLocalFile(large.fileName())});
        QCOMPARE(controller.attachmentNotice(), QStringLiteral("Files up to 16 MB can be sent."));
        QCOMPARE(tray->rowCount(), 0);
        controller.clearAttachmentNotice();
        QCOMPARE(controller.attachmentNotice(), QString());
        QVERIFY(notice.count() >= 4);

        // Ten at a time.
        QList<QUrl> many;
        for (int index = 0; index < AttachmentLimits::maxStaged + 1; ++index)
            many.append(QUrl::fromLocalFile(writeFile(dir, QStringLiteral("n%1.txt").arg(index), "x")));
        controller.attachFiles(many);
        QCOMPARE(tray->rowCount(), AttachmentLimits::maxStaged);
        QCOMPARE(controller.attachmentNotice(), QStringLiteral("Up to 10 attachments can be sent at once."));
        controller.clearStagedAttachments();

        // Nothing is attached to an edit.
        const QString mine = controller.messages()->data(controller.messages()->index(1),
                                                         MessageListModel::StableIdRole).toString();
        QVERIFY(controller.beginEdit(mine));
        controller.attachFiles({many.first()});
        QCOMPARE(tray->rowCount(), 0);
        controller.cancelComposeMode();
    }

    void theTrayBelongsToItsChatAndHidesWhenLocked()
    {
        using namespace OpenChat;
        QTemporaryDir dir;
        const QString photo = writePhoto(dir, QStringLiteral("cat.png"), Qt::red);
        ChatController controller;
        auto *tray = qobject_cast<StagedAttachmentModel *>(controller.stagedAttachments());
        controller.attachFiles({QUrl::fromLocalFile(photo)});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        const QString key = tray->data(tray->index(0), StagedAttachmentModel::PreviewKeyRole).toString();
        QVERIFY(PanelMediaLibrary::instance().contains(key));

        // Locked: the user's own pictures are not on screen either.
        controller.setSessionState(ChatController::SessionState::Locked);
        QCOMPARE(tray->rowCount(), 0);
        QVERIFY(!controller.hasStagedAttachments());
        QVERIFY(!PanelMediaLibrary::instance().contains(key));
        controller.attachFiles({QUrl::fromLocalFile(photo)});
        QCOMPARE(tray->rowCount(), 0);
        QVERIFY(!controller.canSend());
        controller.setSessionState(ChatController::SessionState::Ready);
        QCOMPARE(tray->rowCount(), 1);
        QVERIFY(controller.hasStagedAttachments());
        QVERIFY(PanelMediaLibrary::instance().contains(key));

        // Another chat has a tray of its own.
        QVERIFY(controller.selectContact(QStringLiteral("sarah")));
        QCOMPARE(tray->rowCount(), 0);
        QVERIFY(!PanelMediaLibrary::instance().contains(key));
    }

    void attachmentsAreQuotedAndCopiedByWhatTheyAre()
    {
        using namespace OpenChat;
        ChatController controller;
        MessageListModel *messages = controller.messages();
        const int reference = messages->rowCount();
        controller.injectDemoAttachmentsForCapture();
        QCOMPARE(messages->rowCount(), reference + 5);
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };
        const auto idOf = [&](int attachmentKind, int direction) {
            for (int row = reference; row < messages->rowCount(); ++row) {
                if (role(row, MessageListModel::AttachmentKindRole).toInt() == attachmentKind
                    && role(row, MessageListModel::DirectionRole).toInt() == direction)
                    return role(row, MessageListModel::StableIdRole).toString();
            }
            return QString();
        };
        const QString photo = idOf(int(AttachmentKind::Image), int(MessageDirection::Incoming));
        const QString audio = idOf(int(AttachmentKind::Audio), int(MessageDirection::Incoming));
        const QString file = idOf(int(AttachmentKind::File), int(MessageDirection::Outgoing));
        QVERIFY(!photo.isEmpty() && !audio.isEmpty() && !file.isEmpty());
        // The demo leaves the reference conversation as it was.
        QCOMPARE(role(0, MessageListModel::BodyRole).toString(), QStringLiteral("Hey Daniel!"));
        // One photo still arriving, one waiting in the tray.
        const int arriving =
            rowWhere(messages, MessageListModel::TransferTextRole, QStringLiteral("Receiving… 3 of 7"));
        QVERIFY(arriving >= reference);
        QCOMPARE(role(arriving, MessageListModel::TransferProgressRole).toReal(), 3.0 / 7.0);
        QVERIFY(controller.hasStagedAttachments());

        // A quote says what it answers.
        QVERIFY(controller.beginReply(photo));
        QCOMPARE(controller.composeTargetText(), QStringLiteral("Photo: The view from the ferry this morning"));
        controller.clearStagedAttachments();
        controller.setComposerText(QStringLiteral("Wow"));
        QVERIFY(controller.sendMessage());
        const int reply = messages->rowCount() - 1;
        QCOMPARE(role(reply, MessageListModel::QuotedBodyRole).toString(),
                 QStringLiteral("Photo: The view from the ferry this morning"));
        QVERIFY(controller.beginReply(audio));
        QCOMPARE(controller.composeTargetText(), QStringLiteral("Audio"));
        controller.cancelComposeMode();

        // Copy takes a caption, and there is nothing to take without one;
        // a caption is never edited.
        QVERIFY(controller.copyMessage(photo));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("The view from the ferry this morning"));
        QVERIFY(!controller.copyMessage(audio));
        QVERIFY(!controller.beginEdit(file));

        // Saving: photos and files, to a local file, under the name proposed.
        QCOMPARE(controller.suggestedSaveName(photo), QStringLiteral("IMG_2041.jpg"));
        QCOMPARE(controller.suggestedSaveName(file), QStringLiteral("Trip itinerary.pdf"));
        QTemporaryDir dir;
        const QString target = dir.filePath(QStringLiteral("plan.pdf"));
        QVERIFY(!controller.saveAttachment(file, QUrl(QStringLiteral("https://example.com/plan.pdf"))));
        QVERIFY(!controller.saveAttachment(audio, QUrl::fromLocalFile(target)));
        QVERIFY(controller.saveAttachment(file, QUrl::fromLocalFile(target)));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target), 10'000);
        QFile saved(target);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QVERIFY(saved.readAll().startsWith("%PDF-1.4"));
        QVERIFY(QUrl(controller.attachmentFolderUrl()).isLocalFile());
        // And a photo onto the clipboard as a picture.
        QVERIFY(controller.copyAttachmentImage(photo));
        QTRY_VERIFY_WITH_TIMEOUT(!QGuiApplication::clipboard()->image().isNull(), 10'000);
        QCOMPARE(QGuiApplication::clipboard()->image().size(), QSize(1600, 1200));
        QVERIFY(!controller.copyAttachmentImage(file));
    }

    void attachmentMediaFollowsTheRowAndTheLock()
    {
        using namespace OpenChat;
        ChatController controller;
        MessageListModel *messages = controller.messages();
        controller.injectDemoAttachmentsForCapture();
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };
        const int photoRow = rowWhere(messages, MessageListModel::FileNameRole, QStringLiteral("IMG_2041.jpg"));
        const int arrivingRow = rowWhere(messages, MessageListModel::FileNameRole, QStringLiteral("IMG_2057.jpg"));
        const int audioRow = rowWhere(messages, MessageListModel::AttachmentKindRole, int(AttachmentKind::Audio));
        QVERIFY(photoRow >= 0 && arrivingRow >= 0 && audioRow >= 0);
        PanelMediaLibrary &library = PanelMediaLibrary::instance();

        auto photo = std::make_unique<ChatAttachmentMedia>();
        QSignalSpy changed(photo.get(), &ChatAttachmentMedia::changed);
        photo->setController(&controller);
        photo->setStableId(role(photoRow, MessageListModel::StableIdRole).toString());
        photo->setTransferState(role(photoRow, MessageListModel::TransferStateRole).toInt());
        // The preview at once; the whole photo only when asked for.
        const QString preview = photo->previewKey();
        QVERIFY(!preview.isEmpty());
        QVERIFY(library.contains(preview));
        QVERIFY(photo->imageKey().isEmpty());
        QVERIFY(!photo->ready());
        photo->setWantFull(true);
        QVERIFY(photo->loading());
        QTRY_VERIFY_WITH_TIMEOUT(photo->ready(), 10'000);
        QVERIFY(!photo->loading());
        QVERIFY(changed.count() > 0);
        QVERIFY(library.get(photo->imageKey()).startsWith("\xFF\xD8"));

        // Still arriving: its preview, and nothing more until it is here.
        ChatAttachmentMedia arriving;
        arriving.setController(&controller);
        arriving.setStableId(role(arrivingRow, MessageListModel::StableIdRole).toString());
        arriving.setWantFull(true);
        QVERIFY(!arriving.previewKey().isEmpty());
        QTest::qWait(20);
        QVERIFY(arriving.imageKey().isEmpty());
        QVERIFY(!arriving.ready());
        QVERIFY(!arriving.loading());

        // A song only when the player asks for it.
        ChatAttachmentMedia audio;
        audio.setController(&controller);
        audio.setStableId(role(audioRow, MessageListModel::StableIdRole).toString());
        audio.setTransferState(int(AttachmentTransferState::Ready));
        QVERIFY(audio.songKey().isEmpty());
        audio.setWantSong(true);
        QTRY_VERIFY_WITH_TIMEOUT(audio.ready(), 10'000);
        QVERIFY(!SongLibrary::instance().get(audio.songKey()).isEmpty());
        audio.setWantSong(false);
        QVERIFY(audio.songKey().isEmpty());

        // Locked: every key goes, and nothing is handed out.
        const QString image = photo->imageKey();
        controller.setSessionState(ChatController::SessionState::Locked);
        QVERIFY(photo->previewKey().isEmpty());
        QVERIFY(photo->imageKey().isEmpty());
        QVERIFY(!library.contains(preview));
        QVERIFY(!library.contains(image));
        QVERIFY(controller.attachmentPreview(role(photoRow, MessageListModel::StableIdRole).toString()).isEmpty());
        controller.setSessionState(ChatController::SessionState::Ready);
        QVERIFY(!photo->previewKey().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(photo->ready(), 10'000);

        // A handle releases what it put when it goes.
        const QString again = photo->imageKey();
        photo.reset();
        QVERIFY(!library.contains(again));
        // Safe with no controller at all.
        ChatAttachmentMedia orphan;
        orphan.setStableId(QStringLiteral("0011"));
        orphan.setWantFull(true);
        QVERIFY(orphan.previewKey().isEmpty());
        QVERIFY(!orphan.loading());
    }

    void liveAttachmentsTravelThroughTheEngine()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        MessageListModel *messages = controller.messages();
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };
        QTemporaryDir dir;
        const QByteArray bytes = patternBytes(AttachmentLimits::partBytes + 3'000);
        const QString notes = writeFile(dir, QStringLiteral("notes.bin"), bytes);

        controller.attachFiles({QUrl::fromLocalFile(notes)});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        controller.setComposerText(QStringLiteral("The notes"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(controller.composerText(), QString());
        // Sealed into the store first, then queued: the row follows.
        QTRY_COMPARE_WITH_TIMEOUT(messages->rowCount(), 1, 20'000);
        QCOMPARE(role(0, MessageListModel::KindRole).toInt(), int(MessageKind::Attachment));
        QCOMPARE(role(0, MessageListModel::BodyRole).toString(), QStringLiteral("The notes"));
        QCOMPARE(role(0, MessageListModel::FileNameRole).toString(), QStringLiteral("notes.bin"));
        QCOMPARE(role(0, MessageListModel::TransferStateRole).toInt(), int(AttachmentTransferState::Transferring));
        QCOMPARE(role(0, MessageListModel::TransferTextRole).toString(), QStringLiteral("Sending… 0%"));
        QVERIFY(role(0, MessageListModel::CanCancelRole).toBool());

        // The peer reads what it is, and the key to its bytes.
        QCOMPARE(live.transport->sent.size(), qsizetype(1));
        const CiphertextEnvelopeV1 message = live.transport->sent.at(0);
        QCOMPARE(message.messageKind, EnvelopeMessageKind::MlsPrivateMessage);
        const auto processed = live.peer->process(live.conversation, message.ciphertext);
        QVERIFY(processed.hasValue());
        const auto content = decodeMessageContent(processed.value().applicationData);
        QVERIFY(content && content->attachment);
        QCOMPARE(content->type, MessageContent::Type::Attachment);
        QCOMPARE(content->body, QStringLiteral("The notes"));
        const AttachmentDescriptor descriptor = *content->attachment;
        QCOMPARE(descriptor.byteCount, bytes.size());
        QCOMPARE(descriptor.partCount, 2);

        // Once the relay has the message, its bytes follow, a frame at a time.
        QSignalSpy frames(live.session->syncEngine(), &SyncEngine::messageStateChanged);
        live.transport->onRelayAccepted(message.envelopeId, 1);
        QSet<QByteArray> accepted{message.envelopeId.bytes()};
        quint64 sequence = 1;
        const auto acceptFrames = [&] {
            for (const CiphertextEnvelopeV1 &envelope : live.transport->sent) {
                if (envelope.messageKind == EnvelopeMessageKind::AttachmentControl
                    && !accepted.contains(envelope.envelopeId.bytes())) {
                    accepted.insert(envelope.envelopeId.bytes());
                    live.transport->onRelayAccepted(envelope.envelopeId, ++sequence);
                }
            }
            return role(0, MessageListModel::TransferStateRole).toInt() == int(AttachmentTransferState::Ready);
        };
        QTRY_VERIFY_WITH_TIMEOUT(acceptFrames(), 20'000);
        QCOMPARE(role(0, MessageListModel::TransferTextRole).toString(), QString());
        QVERIFY(!role(0, MessageListModel::CanCancelRole).toBool());
        QVERIFY(role(0, MessageListModel::CanSaveRole).toBool());
        // What went is exactly the file, sealed under the message's key.
        QByteArray reassembled;
        for (const CiphertextEnvelopeV1 &envelope : live.transport->sent) {
            if (envelope.messageKind != EnvelopeMessageKind::AttachmentControl)
                continue;
            QCOMPARE(envelope.recipientDeviceId, live.peerDevice);
            const auto split = splitAttachmentFrame(envelope.ciphertext);
            QVERIFY(split);
            if (split->first.type != AttachmentFrameType::Part)
                continue;
            const auto part = openAttachmentFrame(descriptor.key, envelope.ciphertext);
            QVERIFY(part);
            reassembled += *part;
        }
        QCOMPARE(reassembled, bytes);
        // And it reads back here for the bubble.
        std::optional<QByteArray> loaded;
        controller.loadAttachmentBlob(role(0, MessageListModel::StableIdRole).toString(), &controller,
                                      [&loaded](const QByteArray &blob) { loaded = blob; });
        QTRY_VERIFY_WITH_TIMEOUT(loaded.has_value(), 10'000);
        QCOMPARE(*loaded, bytes);
    }

    void aTextSentRightAfterAttachmentsFollowsThem()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QTemporaryDir dir;
        controller.attachFiles({QUrl::fromLocalFile(writeFile(dir, QStringLiteral("minutes.bin"),
                                                              patternBytes(3 * AttachmentLimits::partBytes))),
                                QUrl::fromLocalFile(writeFile(dir, QStringLiteral("agenda.bin"),
                                                              patternBytes(2'000)))});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        controller.setComposerText(QStringLiteral("The minutes"));
        QVERIFY(controller.sendMessage());
        // Written at once, while those are still being sealed.
        controller.setComposerText(QStringLiteral("These are the signed copies"));
        QVERIFY(controller.sendMessage());
        QCOMPARE(controller.composerText(), QString());

        // Everyone reads them in the order they were sent.
        QTRY_COMPARE_WITH_TIMEOUT(live.transport->sent.size(), qsizetype(3), 20'000);
        QList<MessageContent::Type> order;
        for (const CiphertextEnvelopeV1 &envelope : live.transport->sent) {
            const auto processed = live.peer->process(live.conversation, envelope.ciphertext);
            QVERIFY(processed.hasValue());
            const auto content = decodeMessageContent(processed.value().applicationData);
            QVERIFY(content);
            order.append(content->type);
        }
        QCOMPARE(order, (QList<MessageContent::Type>{MessageContent::Type::Attachment,
                                                     MessageContent::Type::Attachment,
                                                     MessageContent::Type::Text}));
        MessageListModel *messages = controller.messages();
        QTRY_COMPARE_WITH_TIMEOUT(messages->rowCount(), 3, 10'000);
        QCOMPARE(messages->data(messages->index(2), MessageListModel::BodyRole).toString(),
                 QStringLiteral("These are the signed copies"));
    }

    void liveIncomingAttachmentsArriveAndCanBeSaved()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        QSignalSpy notified(&controller, &ChatController::messageNotificationRequested);
        MessageListModel *messages = controller.messages();
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };

        const QByteArray bytes = patternBytes(AttachmentLimits::partBytes + 1'234);
        AttachmentDescriptor descriptor;
        descriptor.key = randomAttachmentKey();
        descriptor.kind = AttachmentKind::File;
        descriptor.byteCount = bytes.size();
        descriptor.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        descriptor.partCount = attachmentPartCount(bytes.size());
        descriptor.fileName = QStringLiteral("report.pdf");
        descriptor.mimeType = QStringLiteral("application/pdf");
        QVERIFY(live.deliverFromPeer(
            EnvelopeMessageKind::MlsPrivateMessage,
            encodeMessageContent(MessageContent::attachmentMessage(QStringLiteral("Q3 numbers"), descriptor))));
        QCOMPARE(messages->rowCount(), 1);
        QCOMPARE(role(0, MessageListModel::KindRole).toInt(), int(MessageKind::Attachment));
        QCOMPARE(role(0, MessageListModel::FileNameRole).toString(), QStringLiteral("report.pdf"));
        QCOMPARE(role(0, MessageListModel::SizeTextRole).toString(), QStringLiteral("225 KB"));
        QCOMPARE(role(0, MessageListModel::TransferTextRole).toString(), QStringLiteral("Waiting for bob"));
        QVERIFY(!role(0, MessageListModel::CanCancelRole).toBool());
        QVERIFY(!role(0, MessageListModel::CanSaveRole).toBool());
        // Announced by what it is.
        QCOMPARE(notified.count(), 1);
        QCOMPARE(notified.first().at(2).toString(), QStringLiteral("report.pdf: Q3 numbers"));

        const auto part = [&](int index) {
            return sealAttachmentFrame(descriptor.key, AttachmentFrameType::Part, descriptor.attachmentId,
                                       quint32(index),
                                       QByteArrayView(bytes).mid(qsizetype(index) * AttachmentLimits::partBytes,
                                                                 attachmentPartSize(descriptor, index)));
        };
        deliver(live, live.peerAccount, live.peerDevice, live.conversation, EnvelopeMessageKind::AttachmentControl,
                part(1));
        QCOMPARE(role(0, MessageListModel::TransferTextRole).toString(), QStringLiteral("Receiving… 1 of 2"));
        QCOMPARE(role(0, MessageListModel::TransferProgressRole).toReal(), 0.5);
        deliver(live, live.peerAccount, live.peerDevice, live.conversation, EnvelopeMessageKind::AttachmentControl,
                part(0));
        QTRY_COMPARE_WITH_TIMEOUT(role(0, MessageListModel::TransferStateRole).toInt(),
                                  int(AttachmentTransferState::Ready), 10'000);
        QVERIFY(role(0, MessageListModel::CanSaveRole).toBool());

        const QString stableId = role(0, MessageListModel::StableIdRole).toString();
        QCOMPARE(controller.suggestedSaveName(stableId), QStringLiteral("report.pdf"));
        QTemporaryDir dir;
        const QString target = dir.filePath(QStringLiteral("report.pdf"));
        QVERIFY(controller.saveAttachment(stableId, QUrl::fromLocalFile(target)));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(target).size() == bytes.size(), 10'000);
        QFile saved(target);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), bytes);

        // History reads it back the same, and a locked profile hands nothing out.
        ChatController reloaded;
        reloaded.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        MessageListModel *history = reloaded.messages();
        QCOMPARE(history->data(history->index(0), MessageListModel::TransferStateRole).toInt(),
                 int(AttachmentTransferState::Ready));
        controller.setSessionState(ChatController::SessionState::Locked);
        std::optional<QByteArray> loaded;
        controller.loadAttachmentBlob(stableId, &controller, [&loaded](const QByteArray &blob) { loaded = blob; });
        QTRY_VERIFY_WITH_TIMEOUT(loaded.has_value(), 10'000);
        QVERIFY(loaded->isEmpty());
        QVERIFY(!controller.saveAttachment(stableId, QUrl::fromLocalFile(target)));
    }

    void liveRetrySendsANewAttachment()
    {
        using namespace OpenChat;
        LiveFixture live;
        QVERIFY(live.setUp());
        QVERIFY(live.acceptPeer(QStringLiteral("bob")));
        ContactRequestService requests(*live.session, *live.session->syncEngine());
        ChatController controller;
        controller.setLiveServices(live.session.get(), live.session->syncEngine(), &requests);
        MessageListModel *messages = controller.messages();
        const auto role = [messages](int row, int role) { return messages->data(messages->index(row), role); };
        QTemporaryDir dir;
        const QByteArray bytes = patternBytes(5'000);
        controller.attachFiles({QUrl::fromLocalFile(writeFile(dir, QStringLiteral("plan.txt"), bytes))});
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy(), 20'000);
        controller.setComposerText(QStringLiteral("The plan"));
        QVERIFY(controller.sendMessage());
        QTRY_COMPARE_WITH_TIMEOUT(messages->rowCount(), 1, 20'000);
        const QString first = role(0, MessageListModel::StableIdRole).toString();

        // Stopped before the relay even had it.
        QVERIFY(controller.cancelAttachment(first));
        QCOMPARE(role(0, MessageListModel::TransferStateRole).toInt(), int(AttachmentTransferState::Cancelled));
        QCOMPARE(role(0, MessageListModel::TransferTextRole).toString(),
                 QStringLiteral("You stopped sending this"));
        QVERIFY(role(0, MessageListModel::CanRetryRole).toBool());
        QVERIFY(!controller.cancelAttachment(first));

        // Again: back in the tray with its caption, then a new attachment.
        QVERIFY(controller.retryAttachment(first));
        QCOMPARE(controller.composerText(), QStringLiteral("The plan"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.stagingBusy() && controller.hasStagedAttachments(), 20'000);
        QVERIFY(controller.sendMessage());
        QTRY_COMPARE_WITH_TIMEOUT(messages->rowCount(), 2, 20'000);
        QVERIFY(role(1, MessageListModel::StableIdRole).toString() != first);

        QVector<AttachmentDescriptor> descriptors;
        for (const CiphertextEnvelopeV1 &envelope : live.transport->sent) {
            if (envelope.messageKind != EnvelopeMessageKind::MlsPrivateMessage)
                continue;
            const auto processed = live.peer->process(live.conversation, envelope.ciphertext);
            QVERIFY(processed.hasValue());
            const auto content = decodeMessageContent(processed.value().applicationData);
            QVERIFY(content && content->attachment);
            descriptors.append(*content->attachment);
            QCOMPARE(content->body, QStringLiteral("The plan"));
        }
        QCOMPARE(descriptors.size(), 2);
        QVERIFY(descriptors.at(0).attachmentId != descriptors.at(1).attachmentId);
        QVERIFY(descriptors.at(0).key != descriptors.at(1).key);
        QCOMPARE(descriptors.at(0).sha256, descriptors.at(1).sha256);
        // The peer was told the first one stopped.
        int cancels = 0;
        for (const CiphertextEnvelopeV1 &envelope : live.transport->sent) {
            const auto split = splitAttachmentFrame(envelope.ciphertext);
            if (envelope.messageKind == EnvelopeMessageKind::AttachmentControl && split
                && split->first.type == AttachmentFrameType::Cancel)
                ++cancels;
        }
        QCOMPARE(cancels, 1);
    }
};

QTEST_MAIN(ChatControllerTest)

#include "tst_chatcontroller.moc"
