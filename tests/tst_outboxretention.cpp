#include "domain/MessageContent.h" // messageIdForCiphertext
#include "network/SyncEngine.h"
#include "storage/RepositorySql.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherSyncStore.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <memory>
#include <optional>

using namespace OpenChat;

// What the outbox keeps once an envelope has settled. A settled envelope is
// never read again, so one with no message row behind it (every control
// envelope: receipts, profile and page updates, call signals, group control,
// edits) is deleted with its ciphertext as it settles; one that backs a
// visible message stays as it always has. Checked on disk through a second
// connection, not through the store's own answers.

namespace {

// Printable, so the inspecting connection can key itself with PRAGMA key.
constexpr char passphrase[] = "0123456789abcdef0123456789abcdef";

// OutboxInspector::state for an envelope that has no row any more.
constexpr int gone = -1;

[[nodiscard]] constexpr int stored(OutboxState state) noexcept
{
    return static_cast<int>(state);
}

// Reads the outbox table through a connection of its own.
class OutboxInspector final
{
public:
    explicit OutboxInspector(const QString &path)
    {
        sqlite3 *handle = nullptr;
        const int opened = sqlite3_open(path.toUtf8().constData(), &handle);
        m_connection.reset(handle); // closes a half-opened handle too
        const QByteArray key = QByteArray("PRAGMA key = '") + passphrase + "';";
        m_keyed = opened == SQLITE_OK && RepositorySql::execute(handle, key.constData());
    }

    [[nodiscard]] bool isReadable() const { return m_keyed && rowCount() >= 0; }

    // Every row the outbox holds, or -2 when the table cannot be read.
    [[nodiscard]] int rowCount() const
    {
        RepositorySql::Statement statement(m_connection.get(), "SELECT count(*) FROM outbox");
        if (!statement.isValid() || sqlite3_step(statement.get()) != SQLITE_ROW)
            return -2;
        return sqlite3_column_int(statement.get(), 0);
    }

    // The envelope's stored state, `gone` once its row is deleted, or -2 when
    // the row cannot be read.
    [[nodiscard]] int state(const EnvelopeId &envelopeId) const
    {
        RepositorySql::Statement statement(m_connection.get(),
                                           "SELECT state FROM outbox WHERE envelope_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, envelopeId.bytes()))
            return -2;
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return gone;
        return step == SQLITE_ROW ? sqlite3_column_int(statement.get(), 0) : -2;
    }

    [[nodiscard]] QByteArray ciphertext(const EnvelopeId &envelopeId) const
    {
        RepositorySql::Statement statement(m_connection.get(),
                                           "SELECT ciphertext FROM outbox WHERE envelope_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, envelopeId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_ROW)
            return {};
        return RepositorySql::blob(statement.get(), 0);
    }

private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_connection{nullptr, &sqlite3_close};
    bool m_keyed = false;
};

// A fresh profile database: the local_profiles row the MLS state's foreign key
// needs, one conversation, the store under test and an inspector. Declared so
// that the inspector and the store go before the database, and the database
// before its directory.
struct Profile final {
    QTemporaryDir directory;
    std::optional<SqlCipherDatabase> database;
    ProfileId profileId = ProfileId::generate();
    ConversationId conversationId = ConversationId::generate();
    std::unique_ptr<SqlCipherChatRepository> chats;
    std::unique_ptr<SqlCipherSyncStore> store;
    std::unique_ptr<OutboxInspector> outbox;

    [[nodiscard]] bool open(SqlCipherSyncStore::Clock clock = {})
    {
        const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
        auto opened = SqlCipherDatabase::open(path, SecureBuffer::fromBytes(passphrase));
        if (!directory.isValid() || !opened.hasValue())
            return false;
        database.emplace(std::move(opened).value());
        auto privateKey = SecureBuffer::fromBytes(QByteArray(32, 's'));
        auto wrappingKey = SecureBuffer::fromBytes(QByteArray(32, 'w'));
        if (!database
                 ->storeDeviceIdentity(profileId, DeviceId::generate(), QByteArray(32, 'p'),
                                       privateKey, wrappingKey, 1'000)
                 .hasValue())
            return false;
        chats = std::make_unique<SqlCipherChatRepository>(*database);
        if (!chats
                 ->upsertConversation(ConversationRecord{conversationId, QByteArray("mls-group"),
                                                         QStringLiteral("Peer"),
                                                         ConversationKind::Direct, 1'000})
                 .hasValue())
            return false;
        store = std::make_unique<SqlCipherSyncStore>(*database, profileId, std::move(clock));
        outbox = std::make_unique<OutboxInspector>(path);
        return outbox->isReadable();
    }

    [[nodiscard]] DeliveryState deliveryState(const MessageId &messageId) const
    {
        const auto history = chats->messages(conversationId, 50, std::nullopt);
        if (history.hasValue()) {
            for (const MessageRecord &message : history.value())
                if (message.id == messageId)
                    return message.deliveryState;
        }
        return DeliveryState::Draft;
    }
};

OutboxRecord envelope(const EnvelopeId &envelopeId, const MessageId &messageId,
                      const ConversationId &conversationId, const QByteArray &ciphertext)
{
    return OutboxRecord{envelopeId, messageId, conversationId, ciphertext, 0,
                        3'000,      0,         OutboxState::Pending};
}

MessageRecord outgoingText(const MessageId &id, const ConversationId &conversationId)
{
    MessageRecord message{id,
                          conversationId,
                          DeviceId::generate(),
                          MessageFlow::Outgoing,
                          ContentKind::Text,
                          QStringLiteral("hello"),
                          2'000,
                          DeliveryState::Queued,
                          std::nullopt,
                          std::nullopt};
    message.sharedId = true;
    return message;
}

// Just enough MLS for the engine to send: encrypt tags the plaintext, and every
// commit carries a state blob for the store's atomic UPSERT. Nothing is received.
class SendOnlyMls final : public SyncMlsSession
{
public:
    Result<QByteArray, MlsError> encrypt(const ConversationId &, QByteArrayView plaintext) override
    {
        return Result<QByteArray, MlsError>::success(QByteArray("ENC:") + plaintext.toByteArray());
    }
    Result<SyncProcessOutcome, MlsError> process(const ConversationId &, QByteArrayView) override
    {
        return Result<SyncProcessOutcome, MlsError>::failure(MlsError::InvalidMessage);
    }
    Result<QList<QByteArray>, MlsError> inspectWelcome(QByteArrayView) override
    {
        return Result<QList<QByteArray>, MlsError>::failure(MlsError::InvalidMessage);
    }
    Result<void, MlsError> joinGroup(const ConversationId &, QByteArrayView) override
    {
        return Result<void, MlsError>::failure(MlsError::InvalidMessage);
    }
    QByteArray takePendingState() override { return QByteArray("mls-state"); }
};

class RecordingTransport final : public SyncTransport
{
public:
    bool isConnected() const override { return true; }
    void sendEnvelope(const CiphertextEnvelopeV1 &envelope) override { sent.append(envelope); }
    void sendDatagram(const CiphertextEnvelopeV1 &) override {}
    void acknowledge(const EnvelopeId &, quint64) override {}

    QVector<CiphertextEnvelopeV1> sent;
};

} // namespace

class OutboxRetentionTest final : public QObject
{
    Q_OBJECT

private slots:
    void acceptedControlSendIsDeleted();
    void failedControlSendIsDeleted();
    void messageBackedRowsAreKept();
    void duplicateAcceptanceStillHarmless();
};

void OutboxRetentionTest::acceptedControlSendIsDeleted()
{
    Profile profile;
    QVERIFY(profile.open());
    SqlCipherSyncStore &store = *profile.store;
    const ConversationId conversation = profile.conversationId;

    // A receipt and a profile update: control envelopes with no message row.
    const auto receipt = EnvelopeId::generate();
    const auto update = EnvelopeId::generate();
    QVERIFY(store.commitControlSend(envelope(receipt, MessageId::generate(), conversation,
                                             QByteArray("receipt envelope")),
                                    QByteArray("s1"))
                .hasValue());
    QVERIFY(store.commitControlSend(envelope(update, MessageId::generate(), conversation,
                                             QByteArray("profile envelope")),
                                    QByteArray("s2"))
                .hasValue());
    QCOMPARE(store.claimDue(3'000, 10, 8'000).value().size(), 2);
    QCOMPARE(profile.outbox->rowCount(), 2);

    // The relay took the receipt: its row, ciphertext and all, is gone, and
    // the other envelope is untouched.
    QVERIFY(store.markAccepted(receipt).hasValue());
    QCOMPARE(profile.outbox->state(receipt), gone);
    QCOMPARE(profile.outbox->state(update), stored(OutboxState::Leased));
    QCOMPARE(profile.outbox->ciphertext(update), QByteArray("profile envelope"));
    QCOMPARE(profile.outbox->rowCount(), 1);

    // Acceptance settles an envelope whatever it was waiting for: one already
    // re-armed for a retry when the relay's answer arrives goes as well.
    QVERIFY(store.scheduleRetry(update, 1, 9'000).hasValue());
    QCOMPARE(profile.outbox->state(update), stored(OutboxState::Pending));
    QVERIFY(store.markAccepted(update).hasValue());
    QCOMPARE(profile.outbox->state(update), gone);
    QCOMPARE(store.claimDue(10'000, 10, 20'000).value().size(), 0);

    // An edit travels as a conversation message but has no row of its own
    // (the row it changes has its own envelope): the edit's envelope goes,
    // the edited message's stays.
    const auto messageId = MessageId::generate();
    const auto text = EnvelopeId::generate();
    QVERIFY(store.commitSend(outgoingText(messageId, conversation),
                             envelope(text, messageId, conversation, QByteArray("text envelope")),
                             QByteArray("s3"))
                .hasValue());
    QCOMPARE(store.claimDue(10'000, 10, 20'000).value().size(), 1);
    QVERIFY(store.advanceDeliveryState(messageId, DeliveryState::Sending).hasValue());
    QVERIFY(store.markAccepted(text).hasValue());
    QVERIFY(store.advanceDeliveryState(messageId, DeliveryState::Sent).hasValue());
    const auto edit = EnvelopeId::generate();
    QVERIFY(store.commitEditSend(conversation, messageId, QStringLiteral("hello again"), 11'000,
                                 {envelope(edit, MessageId::generate(), conversation,
                                           QByteArray("edit envelope"))},
                                 QByteArray("s4"))
                .hasValue());
    QCOMPARE(store.claimDue(12'000, 10, 20'000).value().size(), 1);
    QVERIFY(store.markAccepted(edit).hasValue());
    QCOMPARE(profile.outbox->state(edit), gone);
    QCOMPARE(profile.outbox->state(text), stored(OutboxState::Accepted));
    QCOMPARE(profile.outbox->rowCount(), 1);
}

void OutboxRetentionTest::failedControlSendIsDeleted()
{
    Profile profile;
    QVERIFY(profile.open());
    SqlCipherSyncStore &store = *profile.store;
    const ConversationId conversation = profile.conversationId;

    // A one-to-one control envelope the engine gives up on (attempts spent,
    // expired, or nowhere for the relay to hold it) is failed through
    // failSend with its own message id, which names no row.
    const auto signal = EnvelopeId::generate();
    const auto signalId = MessageId::generate();
    QVERIFY(store.commitControlSend(envelope(signal, signalId, conversation,
                                             QByteArray("call signal envelope")),
                                    QByteArray("s1"))
                .hasValue());
    QCOMPARE(store.claimDue(3'000, 10, 8'000).value().size(), 1);
    QVERIFY(store.failSend(signal, signalId).hasValue());
    QCOMPARE(profile.outbox->state(signal), gone);
    QCOMPARE(profile.outbox->rowCount(), 0);
    // Failing it again is refused, as it was before the row was deleted.
    QVERIFY(!store.failSend(signal, signalId).hasValue());

    // A group control message goes to each member under one message id with
    // no row; each member's envelope is retired, and deleted, on its own.
    const auto groupMessage = MessageId::generate();
    const auto first = EnvelopeId::generate();
    const auto second = EnvelopeId::generate();
    QVERIFY(store.commitControlSendMany(
                     {envelope(first, groupMessage, conversation, QByteArray("roster to a")),
                      envelope(second, groupMessage, conversation, QByteArray("roster to b"))},
                     QByteArray("s2"))
                .hasValue());
    QVERIFY(store.failEnvelope(first).hasValue());
    QCOMPARE(profile.outbox->state(first), gone);
    QCOMPARE(profile.outbox->state(second), stored(OutboxState::Pending));
    QCOMPARE(profile.outbox->ciphertext(second), QByteArray("roster to b"));

    // Retiring it again reports NotFound, exactly as for a retired envelope
    // that is still on disk.
    const auto again = store.failEnvelope(first);
    QVERIFY(!again.hasValue());
    QCOMPARE(again.error().code, RepositoryErrorCode::NotFound);

    QVERIFY(store.failEnvelope(second).hasValue());
    QCOMPARE(profile.outbox->state(second), gone);
    QCOMPARE(profile.outbox->rowCount(), 0);
}

void OutboxRetentionTest::messageBackedRowsAreKept()
{
    Profile profile;
    QVERIFY(profile.open());
    SqlCipherSyncStore &store = *profile.store;
    const ConversationId conversation = profile.conversationId;

    const auto takenId = MessageId::generate();
    const auto taken = EnvelopeId::generate();
    const auto lostId = MessageId::generate();
    const auto lost = EnvelopeId::generate();
    QVERIFY(store.commitSend(outgoingText(takenId, conversation),
                             envelope(taken, takenId, conversation, QByteArray("taken text")),
                             QByteArray("s1"))
                .hasValue());
    QVERIFY(store.commitSend(outgoingText(lostId, conversation),
                             envelope(lost, lostId, conversation, QByteArray("lost text")),
                             QByteArray("s2"))
                .hasValue());
    QCOMPARE(store.claimDue(3'000, 10, 8'000).value().size(), 2);

    // A text the relay took keeps its row, Accepted, ciphertext and all.
    QVERIFY(store.markAccepted(taken).hasValue());
    QCOMPARE(profile.outbox->state(taken), stored(OutboxState::Accepted));
    QCOMPARE(profile.outbox->ciphertext(taken), QByteArray("taken text"));

    // A text that failed keeps its row too, and the message shows Failed.
    QVERIFY(store.failSend(lost, lostId).hasValue());
    QCOMPARE(profile.outbox->state(lost), stored(OutboxState::Failed));
    QCOMPARE(profile.outbox->ciphertext(lost), QByteArray("lost text"));
    QCOMPARE(profile.deliveryState(lostId), DeliveryState::Failed);

    // A group text's envelopes all carry the message's id: one retired for an
    // unreachable member and one taken, both stay.
    const auto groupId = MessageId::generate();
    const auto unreachable = EnvelopeId::generate();
    const auto reached = EnvelopeId::generate();
    QVERIFY(store.commitGroupSend(
                     outgoingText(groupId, conversation),
                     {envelope(unreachable, groupId, conversation, QByteArray("group text a")),
                      envelope(reached, groupId, conversation, QByteArray("group text b"))},
                     QByteArray("s3"))
                .hasValue());
    QVERIFY(store.failEnvelope(unreachable).hasValue());
    QCOMPARE(store.claimDue(3'000, 10, 8'000).value().size(), 1);
    QVERIFY(store.markAccepted(reached).hasValue());
    QCOMPARE(profile.outbox->state(unreachable), stored(OutboxState::Failed));
    QCOMPARE(profile.outbox->state(reached), stored(OutboxState::Accepted));

    // Settling other envelopes never touches one still waiting to go, even a
    // control envelope.
    const auto waiting = EnvelopeId::generate();
    QVERIFY(store.commitControlSend(envelope(waiting, MessageId::generate(), conversation,
                                             QByteArray("waiting receipt")),
                                    QByteArray("s4"))
                .hasValue());
    QVERIFY(store.markAccepted(taken).hasValue());
    QCOMPARE(profile.outbox->state(waiting), stored(OutboxState::Pending));
    QCOMPARE(profile.outbox->rowCount(), 5);
}

void OutboxRetentionTest::duplicateAcceptanceStillHarmless()
{
    qint64 now = 1'700'000'000'000;
    Profile profile;
    QVERIFY(profile.open([&now] { return now; }));
    SqlCipherSyncStore &store = *profile.store;
    const ConversationId conversation = profile.conversationId;

    // The relay can report the same envelope twice (it took a copy re-sent
    // before its first answer arrived). For a control envelope the second
    // report finds no row: it is refused as NotFound and changes nothing.
    const auto control = EnvelopeId::generate();
    const auto waiting = EnvelopeId::generate();
    QVERIFY(store.commitControlSend(envelope(control, MessageId::generate(), conversation,
                                             QByteArray("receipt")),
                                    QByteArray("s1"))
                .hasValue());
    QVERIFY(store.commitControlSend(envelope(waiting, MessageId::generate(), conversation,
                                             QByteArray("next receipt")),
                                    QByteArray("s2"))
                .hasValue());
    QVERIFY(store.markAccepted(control).hasValue());
    const auto duplicate = store.markAccepted(control);
    QVERIFY(!duplicate.hasValue());
    QCOMPARE(duplicate.error().code, RepositoryErrorCode::NotFound);
    QCOMPARE(profile.outbox->state(control), gone);
    QCOMPARE(profile.outbox->state(waiting), stored(OutboxState::Pending));

    // A message's envelope is still there, so its second report succeeds and
    // leaves it as it was.
    const auto textId = MessageId::generate();
    const auto text = EnvelopeId::generate();
    QVERIFY(store.commitSend(outgoingText(textId, conversation),
                             envelope(text, textId, conversation, QByteArray("text")),
                             QByteArray("s3"))
                .hasValue());
    QVERIFY(store.markAccepted(text).hasValue());
    QVERIFY(store.markAccepted(text).hasValue());
    QCOMPARE(profile.outbox->state(text), stored(OutboxState::Accepted));
    QCOMPARE(profile.outbox->ciphertext(text), QByteArray("text"));
    QCOMPARE(profile.outbox->rowCount(), 2);

    // Through the engine: a profile update and a text, each reported twice.
    // Nothing fails closed, the text turns Sent once, only the text's
    // envelope remains, and nothing is ever sent again.
    QVERIFY(store.failEnvelope(waiting).hasValue()); // start from the text's row alone
    QCOMPARE(profile.outbox->rowCount(), 1);
    SendOnlyMls mls;
    RecordingTransport transport;
    SyncEngine engine(SyncEngine::Config{AccountId::generate(), DeviceId::generate()}, store, mls,
                      transport, [](QByteArrayView) { return QByteArray(64, 'S'); },
                      [&now] { return now; });
    int sentReports = 0;
    MessageId reported = MessageId::generate();
    connect(&engine, &SyncEngine::messageStateChanged, &engine,
            [&](const MessageId &messageId, DeliveryState state) {
                if (state == DeliveryState::Sent) {
                    ++sentReports;
                    reported = messageId;
                }
            });
    engine.start();
    const DeviceId peer = DeviceId::generate();
    engine.sendProfileUpdate(conversation, peer, QByteArray("profile"));
    engine.enqueueText(conversation, peer, QStringLiteral("hi"));
    QCOMPARE(transport.sent.size(), 2);
    QCOMPARE(profile.outbox->rowCount(), 3);

    for (int report = 0; report < 2; ++report) {
        for (const CiphertextEnvelopeV1 &sent : std::as_const(transport.sent))
            transport.onRelayAccepted(sent.envelopeId, quint64(10 + report));
    }
    QVERIFY(!engine.isFailedClosed());
    QCOMPARE(sentReports, 1);
    const MessageId hiId = messageIdForCiphertext(transport.sent.at(1).ciphertext);
    QCOMPARE(reported, hiId);
    QCOMPARE(profile.deliveryState(hiId), DeliveryState::Sent);
    QCOMPARE(profile.outbox->state(transport.sent.at(0).envelopeId), gone);
    QCOMPARE(profile.outbox->state(transport.sent.at(1).envelopeId),
             stored(OutboxState::Accepted));
    QCOMPARE(profile.outbox->rowCount(), 2); // the earlier text and this one

    now += 60LL * 60 * 1000;
    transport.onConnected();
    QCOMPARE(transport.sent.size(), 2);
}

QTEST_GUILESS_MAIN(OutboxRetentionTest)
#include "tst_outboxretention.moc"
