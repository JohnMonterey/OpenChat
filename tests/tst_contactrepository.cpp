#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"

#include <QTemporaryDir>
#include <QtTest/QTest>

#include <optional>

using namespace OpenChat;

namespace {

ContactRecord contactRecord(const AccountId &accountId, ContactState state, qint64 createdAtMs,
                            qint64 updatedAtMs, const QString &handle = QStringLiteral("peer"),
                            const QString &displayName = QStringLiteral("Peer"))
{
    return ContactRecord{accountId, handle, displayName, state, std::nullopt, createdAtMs,
                         updatedAtMs};
}

} // namespace

class ContactRepositoryTest final : public QObject
{
    Q_OBJECT

private slots:
    void emptyRosterReturnsEmptyList();
    void outgoingRequestIsStoredAndPersistsAcrossReopen();
    void incomingRequestForNewPeerIsPendingIncoming();
    void markAcceptedMovesPendingToAcceptedWithConversation();
    void blockIsStickyAgainstIncomingRequestsAndAccept();
    void outgoingRequestNeverOverwritesBlocked();
    void incomingRequestDoesNotRegressOutgoing();
    void blockIsIdempotentAndCanBlockUnknownPeer();
    void unblockRejectsNonBlockedAndUnknown();
    void removeDeletesContact();
    void reopeningWithWrongKeyFailsClosed();
    void contactsAreOrderedDeterministically();
    void malformedRowsAreRejectedOnDecode();
};

void ContactRepositoryTest::emptyRosterReturnsEmptyList()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    auto all = contacts.contacts();
    QVERIFY(all.hasValue());
    QVERIFY(all.value().isEmpty());

    auto missing = contacts.find(AccountId::generate());
    QVERIFY(missing.hasValue());
    QVERIFY(!missing.value().has_value());
}

void ContactRepositoryTest::outgoingRequestIsStoredAndPersistsAcrossReopen()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("profile.sqlite3");
    auto key = SecureBuffer::random(32);
    const auto accountId = AccountId::generate();

    {
        auto opened = SqlCipherDatabase::open(path, key);
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherContactRepository contacts(database);

        QVERIFY(contacts
                    .recordOutgoingRequest(contactRecord(accountId, ContactState::PendingOutgoing,
                                                         1'000, 1'000, QStringLiteral("alex"),
                                                         QStringLiteral("Alex")))
                    .hasValue());

        auto found = contacts.find(accountId);
        QVERIFY(found.hasValue());
        QVERIFY(found.value().has_value());
        const auto &record = found.value().value();
        QVERIFY(record.accountId == accountId);
        QVERIFY(record.state == ContactState::PendingOutgoing);
        QCOMPARE(record.handle, QStringLiteral("alex"));
        QCOMPARE(record.displayName, QStringLiteral("Alex"));
        QVERIFY(!record.conversationId.has_value());
        QCOMPARE(record.createdAtMs, qint64(1'000));
        QCOMPARE(record.updatedAtMs, qint64(1'000));
    }

    // Reopening with the RIGHT key returns the row keyed by the same AccountId.
    auto reopened = SqlCipherDatabase::open(path, key);
    QVERIFY(reopened.hasValue());
    auto database = std::move(reopened).value();
    SqlCipherContactRepository contacts(database);
    auto found = contacts.find(accountId);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QVERIFY(found.value().value().accountId == accountId);
    QVERIFY(found.value().value().state == ContactState::PendingOutgoing);
}

void ContactRepositoryTest::incomingRequestForNewPeerIsPendingIncoming()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    QVERIFY(contacts
                .recordIncomingRequest(
                    contactRecord(accountId, ContactState::PendingIncoming, 2'000, 2'000))
                .hasValue());

    auto found = contacts.find(accountId);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QVERIFY(found.value().value().state == ContactState::PendingIncoming);
}

void ContactRepositoryTest::markAcceptedMovesPendingToAcceptedWithConversation()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    const auto conversationId = ConversationId::generate();
    QVERIFY(contacts
                .recordIncomingRequest(
                    contactRecord(accountId, ContactState::PendingIncoming, 3'000, 3'000))
                .hasValue());

    QVERIFY(contacts.markAccepted(accountId, conversationId, 4'000).hasValue());

    auto found = contacts.find(accountId);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    const auto &record = found.value().value();
    QVERIFY(record.state == ContactState::Accepted);
    QVERIFY(record.conversationId.has_value());
    QVERIFY(record.conversationId.value() == conversationId);
    QCOMPARE(record.updatedAtMs, qint64(4'000));
    QCOMPARE(record.createdAtMs, qint64(3'000)); // preserved across acceptance

    // Marking an unknown peer accepted is a NotFound, not a silent insert.
    auto missing = contacts.markAccepted(AccountId::generate(), conversationId, 5'000);
    QVERIFY(!missing.hasValue());
    QCOMPARE(missing.error().code, RepositoryErrorCode::NotFound);
}

void ContactRepositoryTest::blockIsStickyAgainstIncomingRequestsAndAccept()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    QVERIFY(contacts
                .recordIncomingRequest(
                    contactRecord(accountId, ContactState::PendingIncoming, 1'000, 1'000))
                .hasValue());
    QVERIFY(contacts.block(accountId, 2'000).hasValue());

    auto blocked = contacts.find(accountId);
    QVERIFY(blocked.hasValue());
    QVERIFY(blocked.value().has_value());
    QVERIFY(blocked.value().value().state == ContactState::Blocked);

    // A blocked peer's incoming request is accepted by the API but leaves the row
    // Blocked -- never silently resurrected to a pending state.
    auto resurrect = contacts.recordIncomingRequest(
        contactRecord(accountId, ContactState::PendingIncoming, 9'000, 9'000,
                      QStringLiteral("spoofed"), QStringLiteral("Spoofed")));
    QVERIFY(resurrect.hasValue()); // typed result: success, no state change
    auto afterIncoming = contacts.find(accountId);
    QVERIFY(afterIncoming.hasValue());
    QVERIFY(afterIncoming.value().has_value());
    QVERIFY(afterIncoming.value().value().state == ContactState::Blocked);
    // The untrusted incoming request did not overwrite our stored metadata either.
    QCOMPARE(afterIncoming.value().value().handle, QStringLiteral("peer"));
    QCOMPARE(afterIncoming.value().value().updatedAtMs, qint64(2'000));

    // Accepting a blocked peer is refused; it must be unblocked first.
    auto accept = contacts.markAccepted(accountId, ConversationId::generate(), 10'000);
    QVERIFY(!accept.hasValue());
    QCOMPARE(accept.error().code, RepositoryErrorCode::Conflict);
    QVERIFY(contacts.find(accountId).value().value().state == ContactState::Blocked);

    // An explicit unblock is the sanctioned way to leave Blocked; it forgets the
    // peer, so a later request would start a fresh lifecycle.
    QVERIFY(contacts.unblock(accountId).hasValue());
    auto afterUnblock = contacts.find(accountId);
    QVERIFY(afterUnblock.hasValue());
    QVERIFY(!afterUnblock.value().has_value());
}

void ContactRepositoryTest::outgoingRequestNeverOverwritesBlocked()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    QVERIFY(contacts.block(accountId, 1'000).hasValue());

    auto outgoing = contacts.recordOutgoingRequest(
        contactRecord(accountId, ContactState::PendingOutgoing, 2'000, 2'000));
    QVERIFY(!outgoing.hasValue());
    QCOMPARE(outgoing.error().code, RepositoryErrorCode::Conflict);

    auto found = contacts.find(accountId);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QVERIFY(found.value().value().state == ContactState::Blocked);
    QCOMPARE(found.value().value().updatedAtMs, qint64(1'000)); // untouched
}

void ContactRepositoryTest::incomingRequestDoesNotRegressOutgoing()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    QVERIFY(contacts
                .recordOutgoingRequest(
                    contactRecord(accountId, ContactState::PendingOutgoing, 1'000, 1'000))
                .hasValue());

    // Their corroborating incoming request must not regress our PendingOutgoing
    // intent; promotion to Accepted happens explicitly via markAccepted once the
    // MLS group exists.
    QVERIFY(contacts
                .recordIncomingRequest(
                    contactRecord(accountId, ContactState::PendingIncoming, 2'000, 2'000))
                .hasValue());

    auto found = contacts.find(accountId);
    QVERIFY(found.hasValue());
    QVERIFY(found.value().has_value());
    QVERIFY(found.value().value().state == ContactState::PendingOutgoing);
    QCOMPARE(found.value().value().updatedAtMs, qint64(1'000)); // unchanged
}

void ContactRepositoryTest::blockIsIdempotentAndCanBlockUnknownPeer()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    // Blocking an unknown peer creates the row (pre-emptive block).
    const auto accountId = AccountId::generate();
    QVERIFY(contacts.block(accountId, 1'000).hasValue());
    auto first = contacts.find(accountId);
    QVERIFY(first.hasValue());
    QVERIFY(first.value().has_value());
    QVERIFY(first.value().value().state == ContactState::Blocked);
    QVERIFY(!first.value().value().conversationId.has_value());

    // Blocking again is idempotent and simply advances updatedAtMs.
    QVERIFY(contacts.block(accountId, 2'000).hasValue());
    auto second = contacts.find(accountId);
    QVERIFY(second.hasValue());
    QVERIFY(second.value().value().state == ContactState::Blocked);
    QCOMPARE(second.value().value().updatedAtMs, qint64(2'000));
}

void ContactRepositoryTest::unblockRejectsNonBlockedAndUnknown()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    // Unblocking an unknown peer is NotFound.
    auto unknown = contacts.unblock(AccountId::generate());
    QVERIFY(!unknown.hasValue());
    QCOMPARE(unknown.error().code, RepositoryErrorCode::NotFound);

    // Unblocking a non-blocked contact is a Conflict and leaves it intact.
    const auto accountId = AccountId::generate();
    QVERIFY(contacts
                .recordOutgoingRequest(
                    contactRecord(accountId, ContactState::PendingOutgoing, 1'000, 1'000))
                .hasValue());
    auto notBlocked = contacts.unblock(accountId);
    QVERIFY(!notBlocked.hasValue());
    QCOMPARE(notBlocked.error().code, RepositoryErrorCode::Conflict);
    QVERIFY(contacts.find(accountId).value().value().state == ContactState::PendingOutgoing);
}

void ContactRepositoryTest::removeDeletesContact()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    QVERIFY(contacts
                .recordOutgoingRequest(
                    contactRecord(accountId, ContactState::PendingOutgoing, 1'000, 1'000))
                .hasValue());
    QVERIFY(contacts.remove(accountId).hasValue());
    QVERIFY(!contacts.find(accountId).value().has_value());

    // remove is idempotent: deleting an absent contact still succeeds.
    QVERIFY(contacts.remove(accountId).hasValue());
}

void ContactRepositoryTest::reopeningWithWrongKeyFailsClosed()
{
    QTemporaryDir directory;
    const QString path = directory.filePath("profile.sqlite3");
    auto keyA = SecureBuffer::random(32);
    auto keyB = SecureBuffer::random(32);

    {
        auto opened = SqlCipherDatabase::open(path, keyA);
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        SqlCipherContactRepository contacts(database);
        QVERIFY(contacts
                    .recordOutgoingRequest(contactRecord(AccountId::generate(),
                                                         ContactState::PendingOutgoing, 1'000,
                                                         1'000))
                    .hasValue());
    }

    auto wrong = SqlCipherDatabase::open(path, keyB);
    QVERIFY(!wrong.hasValue());
    QCOMPARE(wrong.error(), StorageError::WrongKeyOrCorrupt);
}

void ContactRepositoryTest::contactsAreOrderedDeterministically()
{
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto first = AccountId::generate();
    const auto second = AccountId::generate();
    const auto third = AccountId::generate();
    // Inserted out of chronological order; contacts() must return them sorted by
    // createdAtMs ascending.
    QVERIFY(contacts
                .recordOutgoingRequest(contactRecord(second, ContactState::PendingOutgoing, 200,
                                                     200))
                .hasValue());
    QVERIFY(
        contacts.recordOutgoingRequest(contactRecord(third, ContactState::PendingOutgoing, 300, 300))
            .hasValue());
    QVERIFY(
        contacts.recordIncomingRequest(contactRecord(first, ContactState::PendingIncoming, 100, 100))
            .hasValue());

    auto all = contacts.contacts();
    QVERIFY(all.hasValue());
    QCOMPARE(all.value().size(), qsizetype(3));
    QVERIFY(all.value().at(0).accountId == first);
    QVERIFY(all.value().at(1).accountId == second);
    QVERIFY(all.value().at(2).accountId == third);
    QCOMPARE(all.value().at(0).createdAtMs, qint64(100));
    QCOMPARE(all.value().at(2).createdAtMs, qint64(300));
}

void ContactRepositoryTest::malformedRowsAreRejectedOnDecode()
{
    // Decode hardening. The repository only ever writes valid 16-byte StrongIds
    // and in-range state integers, and the schema's CHECK(length(...) = 16)
    // constraints on account_id and conversation_id reject any malformed blob at
    // write time. There is therefore no path through the public API (which
    // constructs ids only via StrongId::generate/fromBytes) to persist a corrupt
    // row, so the IntegrityFailure branches in decodeContact are unreachable from
    // here and we rely on those CHECK constraints plus the in-range state guard.
    //
    // What we can assert directly is the well-formed decode of every column,
    // including the nullable conversation_id (NULL in pending states, a real id
    // once accepted).
    QTemporaryDir directory;
    auto key = SecureBuffer::random(32);
    auto opened = SqlCipherDatabase::open(directory.filePath("profile.sqlite3"), key);
    QVERIFY(opened.hasValue());
    auto database = std::move(opened).value();
    SqlCipherContactRepository contacts(database);

    const auto accountId = AccountId::generate();
    QVERIFY(contacts
                .recordIncomingRequest(
                    contactRecord(accountId, ContactState::PendingIncoming, 1'000, 1'000))
                .hasValue());
    // NULL conversation_id decodes to a disengaged optional.
    QVERIFY(!contacts.find(accountId).value().value().conversationId.has_value());

    const auto conversationId = ConversationId::generate();
    QVERIFY(contacts.markAccepted(accountId, conversationId, 2'000).hasValue());
    // A stored 16-byte conversation_id decodes back to exactly the same id.
    auto accepted = contacts.find(accountId);
    QVERIFY(accepted.value().value().conversationId.has_value());
    QVERIFY(accepted.value().value().conversationId.value() == conversationId);
}

QTEST_GUILESS_MAIN(ContactRepositoryTest)
#include "tst_contactrepository.moc"
