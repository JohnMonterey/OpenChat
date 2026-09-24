#include "app/ProfileSession.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/RepositorySql.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherDatabase.h"
#include "storage/SqlCipherProfilePageRepository.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <limits>
#include <memory>
#include <optional>

using namespace OpenChat;

namespace {

// A passphrase key, so a raw SQLCipher connection (PRAGMA key) opens the same
// file SqlCipherDatabase::open does (sqlite3_key with the same bytes).
constexpr char databaseKeyText[] = "0123456789abcdef0123456789abcdef";

SecureBuffer databaseKey()
{
    return SecureBuffer::fromBytes(QByteArrayView(databaseKeyText));
}

const QStringList pageTables{
    QStringLiteral("profile_media"),   QStringLiteral("local_profile_page"),
    QStringLiteral("contact_pages"),   QStringLiteral("contact_page_media"),
    QStringLiteral("page_deliveries"), QStringLiteral("page_delivery_media"),
    QStringLiteral("page_requests")};

constexpr int backgroundKind = 1;
constexpr int songKind = 2;
constexpr qsizetype maxBlobBytes = 224 * 1024;
constexpr qint64 never = std::numeric_limits<qint64>::max();

QByteArray sha256(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

// The code of a call that failed, or none when it succeeded: comparing this
// keeps a wrongly accepted call a test failure rather than a throw from
// Result::error().
template<typename T>
std::optional<RepositoryErrorCode> errorCode(const Result<T, RepositoryError> &result)
{
    if (result.hasValue())
        return std::nullopt;
    return result.error().code;
}

const std::optional<RepositoryErrorCode> invalidInput = RepositoryErrorCode::InvalidInput;
const std::optional<RepositoryErrorCode> conflict = RepositoryErrorCode::Conflict;

struct Blob final {
    QByteArray data;
    QByteArray hash;
};

Blob blob(char fill, qsizetype size = 1'000)
{
    QByteArray data(size, fill);
    return Blob{data, sha256(data)};
}

// A second connection to a profile database, for what the production API
// deliberately does not offer: building an old schema, reading the schema
// back and writing rows that bypass the repository's own checks.
class RawDatabase final
{
public:
    explicit RawDatabase(const QString &path)
    {
        if (sqlite3_open(QFile::encodeName(path).constData(), &m_handle) != SQLITE_OK
            || !exec(QByteArrayLiteral("PRAGMA key = '") + databaseKeyText + "';")) {
            sqlite3_close(m_handle);
            m_handle = nullptr;
        }
    }

    ~RawDatabase()
    {
        if (m_handle)
            sqlite3_close(m_handle);
    }

    RawDatabase(const RawDatabase &) = delete;
    RawDatabase &operator=(const RawDatabase &) = delete;

    [[nodiscard]] bool isOpen() const { return m_handle != nullptr; }
    [[nodiscard]] sqlite3 *get() const { return m_handle; }

    bool exec(const QByteArray &sql) const
    {
        return m_handle && RepositorySql::execute(m_handle, sql.constData());
    }

    // Runs `sql` expecting it to be refused by a constraint, not by a typo:
    // a statement that fails for any other reason proves nothing.
    [[nodiscard]] bool violatesConstraint(const QByteArray &sql) const
    {
        return !exec(sql) && (sqlite3_extended_errcode(m_handle) & 0xFF) == SQLITE_CONSTRAINT;
    }

    // The first column of the first row, or -1 when there is no row.
    [[nodiscard]] qint64 integer(const QByteArray &sql) const
    {
        RepositorySql::Statement statement(m_handle, sql.constData());
        if (!statement.isValid() || sqlite3_step(statement.get()) != SQLITE_ROW)
            return -1;
        return sqlite3_column_int64(statement.get(), 0);
    }

    // Every row's value of column `column`, as text.
    [[nodiscard]] QStringList texts(const QByteArray &sql, int column = 0) const
    {
        QStringList values;
        RepositorySql::Statement statement(m_handle, sql.constData());
        while (statement.isValid() && sqlite3_step(statement.get()) == SQLITE_ROW)
            values.append(RepositorySql::text(statement.get(), column));
        return values;
    }

    [[nodiscard]] QStringList columns(const QString &table) const
    {
        return texts("PRAGMA table_info(" + table.toLatin1() + ")", 1);
    }

    [[nodiscard]] QStringList foreignKeyTargets(const QString &table) const
    {
        return texts("PRAGMA foreign_key_list(" + table.toLatin1() + ")", 2);
    }

    [[nodiscard]] bool hasTable(const QString &table) const
    {
        return integer("SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = '"
                       + table.toLatin1() + "'")
            == 1;
    }

private:
    sqlite3 *m_handle = nullptr;
};

// Writes the file a client at schema `version` left behind, by running the
// shipped migrations 001..version from the resources exactly as migrate()
// does (one transaction, foreign keys on).
bool createAtVersion(const QString &path, int version)
{
    RawDatabase raw(path);
    if (!raw.isOpen() || !raw.exec("PRAGMA foreign_keys = ON;") || !raw.exec("BEGIN IMMEDIATE;"))
        return false;
    const QStringList files = QDir(QStringLiteral(":/openchat"))
                                  .entryList({QStringLiteral("*.sql")}, QDir::Files, QDir::Name);
    int applied = 0;
    for (const QString &file : files) {
        const int number = file.left(3).toInt();
        if (number < 1 || number > version)
            continue;
        QFile migration(QStringLiteral(":/openchat/") + file);
        if (!migration.open(QIODevice::ReadOnly) || !raw.exec(migration.readAll()))
            return false;
        ++applied;
    }
    return applied == version && raw.exec("COMMIT;")
        && raw.integer("PRAGMA user_version") == version;
}

bool insertLocalProfile(const RawDatabase &raw, const ProfileId &profileId)
{
    RepositorySql::Statement statement(
        raw.get(),
        "INSERT INTO local_profiles(profile_id, device_id, signing_public_key, private_nonce, "
        "private_ciphertext, private_tag, created_at_ms, display_name) "
        "VALUES(?1, randomblob(16), randomblob(32), randomblob(12), randomblob(32), "
        "randomblob(16), 1000, 'Ada')");
    return statement.isValid() && statement.bindBlob(1, profileId.bytes())
        && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// A conversation with one message, in the columns every schema since 003 has.
bool insertMessage(const RawDatabase &raw, const ConversationId &conversation,
                   const MessageId &message)
{
    RepositorySql::Statement conversationRow(
        raw.get(),
        "INSERT OR IGNORE INTO conversations(id, kind, title, created_at_ms) "
        "VALUES(?1, 0, 'Michael', 1000)");
    RepositorySql::Statement messageRow(
        raw.get(),
        "INSERT INTO messages(id, conversation_id, sender_device_id, content_kind, content, "
        "client_created_at_ms, delivery_state, flow, body, sent_at_ms) "
        "VALUES(?1, ?2, randomblob(16), 0, X'6869', 2000, 3, 1, 'hi', 2000)");
    return conversationRow.isValid() && conversationRow.bindBlob(1, conversation.bytes())
        && sqlite3_step(conversationRow.get()) == SQLITE_DONE && messageRow.isValid()
        && messageRow.bindBlob(1, message.bytes()) && messageRow.bindBlob(2, conversation.bytes())
        && sqlite3_step(messageRow.get()) == SQLITE_DONE;
}

bool insertOutbox(const RawDatabase &raw, const EnvelopeId &envelope, const MessageId &message,
                  int state)
{
    RepositorySql::Statement statement(
        raw.get(),
        "INSERT INTO outbox(envelope_id, message_id, ciphertext, attempt_count, "
        "next_attempt_at_ms, state) VALUES(?1, ?2, X'00', 0, 0, ?3)");
    return statement.isValid() && statement.bindBlob(1, envelope.bytes())
        && statement.bindBlob(2, message.bytes()) && statement.bindInt(3, state)
        && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// One profile database with its local profile row (which the local page
// references) and the repositories the tests drive.
class PageStore final
{
public:
    [[nodiscard]] bool open()
    {
        if (!m_directory.isValid())
            return false;
        auto opened = SqlCipherDatabase::open(path(), databaseKey());
        if (!opened.hasValue())
            return false;
        m_database = std::make_unique<SqlCipherDatabase>(std::move(opened).value());
        if (!m_created) {
            if (!m_database
                     ->storeDeviceIdentity(profileId, DeviceId::generate(), QByteArray(32, 'k'),
                                           SecureBuffer::random(32), SecureBuffer::random(32),
                                           1'000)
                     .hasValue())
                return false;
            m_created = true;
        }
        pages = std::make_unique<SqlCipherProfilePageRepository>(*m_database, profileId);
        contacts = std::make_unique<SqlCipherContactRepository>(*m_database);
        return true;
    }

    [[nodiscard]] bool reopen()
    {
        close();
        return open();
    }

    void close()
    {
        pages.reset();
        contacts.reset();
        m_database.reset();
    }

    [[nodiscard]] QString path() const
    {
        return m_directory.filePath(QStringLiteral("profile.sqlite3"));
    }

    [[nodiscard]] SqlCipherDatabase &database() { return *m_database; }

    // A roster row in `state`; Accepted rows get a conversation like a real
    // accepted contact.
    [[nodiscard]] bool addContact(const AccountId &account, ContactState state)
    {
        ContactRecord record{account, QStringLiteral("peer"), QStringLiteral("Peer"), state};
        record.createdAtMs = 1'000;
        record.updatedAtMs = 1'000;
        switch (state) {
        case ContactState::PendingOutgoing:
            return contacts->recordOutgoingRequest(record).hasValue();
        case ContactState::PendingIncoming:
            return contacts->recordIncomingRequest(record).hasValue();
        case ContactState::Accepted:
            return contacts->recordIncomingRequest(record).hasValue()
                && contacts->markAccepted(account, ConversationId::generate(), 2'000).hasValue();
        case ContactState::Blocked:
            return contacts->block(account, 2'000).hasValue();
        }
        return false;
    }

    // `from` sends `media` as `kind`, arriving at `atMs`.
    [[nodiscard]] bool receive(const AccountId &from, int kind, const Blob &media, qint64 atMs)
    {
        return pages->putContactMedia(from, kind, media.hash, media.data, atMs).hasValue();
    }

    // The editor imports `media` into the draft's `kind` slot at `atMs`.
    [[nodiscard]] bool importIntoDraft(int kind, const Blob &media, qint64 atMs)
    {
        return pages->putLocalDraftMedia(kind, media.hash, media.data, atMs).hasValue();
    }

    // How many blobs are stored, whoever references them.
    [[nodiscard]] qint64 blobCount() const
    {
        RawDatabase raw(path());
        return raw.integer("SELECT count(*) FROM profile_media");
    }

    ProfileId profileId = ProfileId::generate();
    std::unique_ptr<SqlCipherProfilePageRepository> pages;
    std::unique_ptr<SqlCipherContactRepository> contacts;

private:
    QTemporaryDir m_directory;
    std::unique_ptr<SqlCipherDatabase> m_database;
    bool m_created = false;
};

StoredContactPage contactPage(const AccountId &account, qint64 revision, const QByteArray &core,
                              std::optional<QByteArray> background = std::nullopt,
                              std::optional<QByteArray> song = std::nullopt,
                              qint64 receivedAtMs = 5'000)
{
    StoredContactPage page{account};
    page.revision = revision;
    page.core = core;
    page.background = std::move(background);
    page.song = std::move(song);
    page.receivedAtMs = receivedAtMs;
    return page;
}

// An in-memory vault for ProfileSession: one database key and one wrapping
// key, created on demand.
class MemoryVault final : public KeyVault
{
public:
    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }

    Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &) override
    {
        return read(m_databaseKey);
    }

    Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &) override
    {
        return create(m_databaseKey);
    }

    Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void, KeyVaultError>::success();
    }

    Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &) override
    {
        return read(m_wrappingKey);
    }

    Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &) override
    {
        return create(m_wrappingKey);
    }

    Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void, KeyVaultError>::success();
    }

private:
    static Result<SecureBuffer, KeyVaultError> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }

    static Result<SecureBuffer, KeyVaultError> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

} // namespace

class ProfilePageStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void migrationFromVersion15AddsPageTablesAndHandle();
    void migrationFromVersion13StillWorks();
    void migrationPrunesSettledControlEnvelopes();
    void pageTablesHaveNoForeignKeyToContacts();
    void localHandlePersistsAcrossUnlock();
    void setHandleRejectsNonCanonical();
    void draftAndPublishedRoundTrip();
    void savePublishedClearsDraft();
    void localDraftMediaIsReferencedAtOnce();
    void localDraftMediaIsAllOrNothing();
    void mediaIsStoredOnceByHash();
    void contactMediaIsScopedByOwnerAndKind();
    void pendingContactMediaCountsOnlyUnnamedRows();
    void contactPageStoresOnlyNewerRevisions();
    void garbageCollectionKeepsEveryReferencedBlob();
    void collectionDropsRowsOfNonContacts();
    void pendingMediaExpires();
    void localBlobGraceRunsFromWhenItIsLetGo();
    void replacedCoreForgetsItsMedia();
    void deliveryDeviceAndCapabilityRoundTrip();
    void forgetSentMediaForOneAccountAndExcept();
    void requestStateRoundTrip();
    void evictContactMediaKeepsTheCoreAndSharedBlobs();
    void leastRecentlyViewedOrdersByViewThenArrival();
    void receivedMediaBytesCountsOnlyContactMedia();
    void checkConstraintsRejectBadHashesAndOversizeBlobs();
};

void ProfilePageStoreTest::migrationFromVersion15AddsPageTablesAndHandle()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
    const auto profileId = ProfileId::generate();
    QVERIFY(createAtVersion(path, 15));
    {
        RawDatabase raw(path);
        QVERIFY(raw.isOpen());
        QVERIFY(insertLocalProfile(raw, profileId));
        // The fixture really is a 0.2.8 file: none of 016 is there yet.
        QVERIFY(!raw.columns(QStringLiteral("local_profiles")).contains(QStringLiteral("handle")));
        for (const QString &table : pageTables)
            QVERIFY2(!raw.hasTable(table), qPrintable(table));
    }

    {
        auto opened = SqlCipherDatabase::open(path, databaseKey());
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        // The existing profile keeps its data and reads back an empty handle.
        QCOMPARE(database.loadProfileDisplayName(profileId).value(), QStringLiteral("Ada"));
        const auto handle = database.loadProfileHandle(profileId);
        QVERIFY(handle.hasValue());
        QVERIFY(handle.value().isEmpty());
        QVERIFY(database.storeProfileHandle(profileId, QStringLiteral("ada")).hasValue());
        QCOMPARE(database.loadProfileHandle(profileId).value(), QStringLiteral("ada"));
        // An unknown profile is NotFound, not a silent success.
        const auto unknown
            = database.storeProfileHandle(ProfileId::generate(), QStringLiteral("x.y"));
        QVERIFY(!unknown.hasValue());
        QCOMPARE(unknown.error(), StorageError::NotFound);

        // The repository works on the migrated file.
        SqlCipherProfilePageRepository pages(database, profileId);
        QVERIFY(pages.saveDraft(QByteArray("core"), std::nullopt, std::nullopt, QString(), 7)
                    .hasValue());
        QCOMPARE(pages.localPage().value().draftCore, QByteArray("core"));
    }

    RawDatabase raw(path);
    QVERIFY(raw.isOpen());
    QCOMPARE(raw.integer("PRAGMA user_version"), qint64(16));
    QVERIFY(raw.columns(QStringLiteral("local_profiles")).contains(QStringLiteral("handle")));
    // The exact 016 schema, column by column.
    QCOMPARE(raw.columns(QStringLiteral("profile_media")),
             QStringList({"sha256", "kind", "data", "created_at_ms"}));
    QCOMPARE(raw.columns(QStringLiteral("local_profile_page")),
             QStringList({"profile_id", "draft_core", "draft_background", "draft_song",
                          "draft_song_source", "draft_updated_at_ms", "published_core",
                          "published_revision", "published_background", "published_song",
                          "published_at_ms"}));
    QCOMPARE(raw.columns(QStringLiteral("contact_pages")),
             QStringList({"account_id", "revision", "core", "background_sha256", "song_sha256",
                          "received_at_ms", "viewed_at_ms"}));
    QCOMPARE(raw.columns(QStringLiteral("contact_page_media")),
             QStringList({"account_id", "sha256", "kind", "received_at_ms"}));
    QCOMPARE(raw.columns(QStringLiteral("page_deliveries")),
             QStringList({"account_id", "device_id", "sent_revision", "sent_at_ms", "page_capable",
                          "last_answer_at_ms", "answer_window_start_ms", "answers_in_window"}));
    QCOMPARE(raw.columns(QStringLiteral("page_delivery_media")),
             QStringList({"account_id", "sha256", "sent_at_ms"}));
    QCOMPARE(raw.columns(QStringLiteral("page_requests")),
             QStringList(
                 {"account_id", "last_request_at_ms", "unanswered", "media_requested_revision"}));
}

void ProfilePageStoreTest::migrationFromVersion13StillWorks()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
    const auto profileId = ProfileId::generate();
    const auto conversation = ConversationId::generate();
    const auto message = MessageId::generate();
    QVERIFY(createAtVersion(path, 13));
    {
        RawDatabase raw(path);
        QVERIFY(raw.isOpen());
        QVERIFY(insertLocalProfile(raw, profileId));
        QVERIFY(insertMessage(raw, conversation, message));
        QVERIFY(!raw.columns(QStringLiteral("messages")).contains(QStringLiteral("locally_read")));
    }

    // 014, 015 and 016 run together, in one transaction.
    {
        auto opened = SqlCipherDatabase::open(path, databaseKey());
        QVERIFY(opened.hasValue());
        auto database = std::move(opened).value();
        QVERIFY(database.loadProfileHandle(profileId).value().isEmpty());
        SqlCipherProfilePageRepository pages(database, profileId);
        QVERIFY(
            pages.savePublished(QByteArray("page"), 3, std::nullopt, std::nullopt, 9).hasValue());
    }

    RawDatabase raw(path);
    QVERIFY(raw.isOpen());
    QCOMPARE(raw.integer("PRAGMA user_version"), qint64(16));
    const QStringList messageColumns = raw.columns(QStringLiteral("messages"));
    QVERIFY(messageColumns.contains(QStringLiteral("locally_read"))); // 014
    QVERIFY(messageColumns.contains(QStringLiteral("quoted_body"))); // 015
    for (const QString &table : pageTables) // 016
        QVERIFY2(raw.hasTable(table), qPrintable(table));
    // The old history survived every step and reads as already read.
    QCOMPARE(raw.integer("SELECT count(*) FROM messages"), qint64(1));
    QCOMPARE(raw.integer("SELECT locally_read FROM messages"), qint64(1));
    QCOMPARE(raw.integer("SELECT published_revision FROM local_profile_page"), qint64(3));
}

void ProfilePageStoreTest::migrationPrunesSettledControlEnvelopes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
    QVERIFY(createAtVersion(path, 15));

    const auto conversation = ConversationId::generate();
    const auto message = MessageId::generate();
    const auto acceptedControl = EnvelopeId::generate();
    const auto failedControl = EnvelopeId::generate();
    const auto acceptedMessage = EnvelopeId::generate();
    const auto failedMessage = EnvelopeId::generate();
    const auto pendingControl = EnvelopeId::generate();
    const auto leasedControl = EnvelopeId::generate();
    {
        RawDatabase raw(path);
        QVERIFY(raw.isOpen());
        QVERIFY(insertMessage(raw, conversation, message));
        // OutboxState: Pending 0, Leased 1, Accepted 2, Failed 3. A control
        // envelope's message_id names no messages row.
        QVERIFY(insertOutbox(raw, acceptedControl, MessageId::generate(), 2));
        QVERIFY(insertOutbox(raw, failedControl, MessageId::generate(), 3));
        QVERIFY(insertOutbox(raw, acceptedMessage, message, 2));
        QVERIFY(insertOutbox(raw, failedMessage, message, 3));
        QVERIFY(insertOutbox(raw, pendingControl, MessageId::generate(), 0));
        QVERIFY(insertOutbox(raw, leasedControl, MessageId::generate(), 1));
        QCOMPARE(raw.integer("SELECT count(*) FROM outbox"), qint64(6));
    }

    {
        auto opened = SqlCipherDatabase::open(path, databaseKey());
        QVERIFY(opened.hasValue());
    }

    RawDatabase raw(path);
    QVERIFY(raw.isOpen());
    const QStringList remaining = raw.texts("SELECT hex(envelope_id) FROM outbox");
    const QSet<QString> expected{acceptedMessage.toHex().toUpper(), failedMessage.toHex().toUpper(),
                                 pendingControl.toHex().toUpper(), leasedControl.toHex().toUpper()};
    QCOMPARE(QSet<QString>(remaining.cbegin(), remaining.cend()), expected);
    QCOMPARE(remaining.size(), 4);
}

void ProfilePageStoreTest::pageTablesHaveNoForeignKeyToContacts()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const Blob picture = blob('a');
    QVERIFY(store.addContact(alice, ContactState::Accepted));
    QVERIFY(store.pages->storeContactPage(contactPage(alice, 1, "core", picture.hash)).value());
    QVERIFY(store.receive(alice, backgroundKind, picture, 10));
    QVERIFY(store.pages->saveDelivery(PageDelivery{alice}).hasValue());
    QVERIFY(store.pages->recordMediaSent(alice, picture.hash, 11).hasValue());
    QVERIFY(store.pages->saveRequestState(PageRequestState{alice}).hasValue());
    store.close();

    RawDatabase raw(store.path());
    QVERIFY(raw.isOpen());
    for (const QString &table : pageTables)
        QVERIFY2(!raw.foreignKeyTargets(table).contains(QStringLiteral("contacts")),
                 qPrintable(table));
    // The pragma really reports foreign keys: the local page's one is there.
    QCOMPARE(raw.foreignKeyTargets(QStringLiteral("local_profile_page")),
             QStringList({QStringLiteral("local_profiles")}));

    // What the rule protects against: a migration that rebuilds contacts
    // drops it with foreign keys on, which would cascade into any child.
    QVERIFY(raw.exec("PRAGMA foreign_keys = ON;"));
    QVERIFY(raw.exec("BEGIN IMMEDIATE;"));
    QVERIFY(raw.exec("DROP TABLE contacts;"));
    QCOMPARE(raw.integer("SELECT count(*) FROM contact_pages"), qint64(1));
    QCOMPARE(raw.integer("SELECT count(*) FROM contact_page_media"), qint64(1));
    QCOMPARE(raw.integer("SELECT count(*) FROM page_deliveries"), qint64(1));
    QCOMPARE(raw.integer("SELECT count(*) FROM page_delivery_media"), qint64(1));
    QCOMPARE(raw.integer("SELECT count(*) FROM page_requests"), qint64(1));
    QVERIFY(raw.exec("ROLLBACK;"));
}

void ProfilePageStoreTest::localHandlePersistsAcrossUnlock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryVault vault;
    const auto profileId = ProfileId::generate();
    const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

    auto created = ProfileSession::create(profileId, vault, paths);
    QVERIFY(created.hasValue());
    auto session = std::move(created).value();
    QCOMPARE(session->profileId(), profileId);
    QVERIFY(session->handle().isEmpty());
    QVERIFY(session->profilePages());
    QVERIFY(session->setHandle(QStringLiteral("ada.lovelace")).hasValue());
    QCOMPARE(session->handle(), QStringLiteral("ada.lovelace"));
    // The session's page store is bound to this profile's own row.
    QVERIFY(session->profilePages()
                ->savePublished(QByteArray("published"), 4, std::nullopt, std::nullopt, 50)
                .hasValue());

    session->lock();
    QVERIFY(session->handle().isEmpty());
    QVERIFY(!session->profilePages());
    QCOMPARE(session->profileId(), profileId);

    auto unlocked = ProfileSession::unlock(profileId, vault, paths);
    QVERIFY(unlocked.hasValue());
    session = std::move(unlocked).value();
    QCOMPARE(session->handle(), QStringLiteral("ada.lovelace"));
    QVERIFY(session->profilePages());
    const auto page = session->profilePages()->localPage();
    QVERIFY(page.hasValue());
    QCOMPARE(page.value().publishedCore, QByteArray("published"));
    QCOMPARE(page.value().publishedRevision, qint64(4));

    // A later reverse lookup can replace it, durably.
    QVERIFY(session->setHandle(QStringLiteral("ada_2")).hasValue());
    session->lock();
    auto again = ProfileSession::unlock(profileId, vault, paths);
    QVERIFY(again.hasValue());
    QCOMPARE(again.value()->handle(), QStringLiteral("ada_2"));
}

void ProfilePageStoreTest::setHandleRejectsNonCanonical()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryVault vault;
    const auto profileId = ProfileId::generate();
    const auto paths = ProfilePaths::forProfile(directory.path(), profileId);

    auto created = ProfileSession::create(profileId, vault, paths);
    QVERIFY(created.hasValue());
    auto session = std::move(created).value();
    QVERIFY(session->setHandle(QStringLiteral("ada")).hasValue());

    const QStringList rejected{QStringLiteral("Ada"),          QStringLiteral("@ada"),
                               QStringLiteral(" ada"),         QStringLiteral("ada "),
                               QStringLiteral("ad"),           QString(),
                               QStringLiteral("ada lovelace"), QStringLiteral("-ada"),
                               QStringLiteral("äda"),          QString(33, QLatin1Char('a'))};
    for (const QString &handle : rejected) {
        const auto result = session->setHandle(handle);
        QVERIFY2(!result.hasValue(), qPrintable(handle));
        QCOMPARE(result.error(), ProfileSessionError::DatabaseFailure);
        QCOMPARE(session->handle(), QStringLiteral("ada"));
    }
    session->lock();
    const auto locked = session->setHandle(QStringLiteral("bob"));
    QVERIFY(!locked.hasValue());
    QCOMPARE(locked.error(), ProfileSessionError::NotUnlocked);

    auto unlocked = ProfileSession::unlock(profileId, vault, paths);
    QVERIFY(unlocked.hasValue());
    QCOMPARE(unlocked.value()->handle(), QStringLiteral("ada"));
}

void ProfilePageStoreTest::draftAndPublishedRoundTrip()
{
    PageStore store;
    QVERIFY(store.open());
    const QByteArray backgroundHash = blob('b').hash;
    const QByteArray songHash = blob('s').hash;
    // Cores are opaque bytes to the store; these start like real page messages.
    const QByteArray publishedCore = QByteArray::fromHex("ff01") + "published";
    const QByteArray draftCore = QByteArray::fromHex("ff02") + "draft";
    const qint64 revision = 1'700'000'000'000;
    const QString songSource
        = QStringLiteral(R"({"path":"/home/ada/Music/tüne.wav","startMs":12000,"lengthMs":45000})");

    // Nothing stored yet reads as the defaults.
    auto fresh = store.pages->localPage();
    QVERIFY(fresh.hasValue());
    QVERIFY(fresh.value().draftCore.isEmpty());
    QVERIFY(!fresh.value().draftBackground && !fresh.value().draftSong);
    QVERIFY(fresh.value().draftSongSource.isEmpty());
    QCOMPARE(fresh.value().draftUpdatedAtMs, qint64(0));
    QVERIFY(fresh.value().publishedCore.isEmpty());
    QCOMPARE(fresh.value().publishedRevision, qint64(0));
    QVERIFY(!fresh.value().publishedBackground && !fresh.value().publishedSong);
    QCOMPARE(fresh.value().publishedAtMs, qint64(0));
    QVERIFY(store.pages->clearDraft(1'000).hasValue()); // harmless without a row

    QVERIFY(store.pages->savePublished(publishedCore, revision, backgroundHash, songHash, 6'000)
                .hasValue());
    QVERIFY(
        store.pages->saveDraft(draftCore, backgroundHash, songHash, songSource, 5'000).hasValue());

    QVERIFY(store.reopen());
    auto page = store.pages->localPage();
    QVERIFY(page.hasValue());
    QCOMPARE(page.value().draftCore, draftCore);
    QCOMPARE(page.value().draftBackground, std::optional<QByteArray>(backgroundHash));
    QCOMPARE(page.value().draftSong, std::optional<QByteArray>(songHash));
    QCOMPARE(page.value().draftSongSource, songSource);
    QCOMPARE(page.value().draftUpdatedAtMs, qint64(5'000));
    QCOMPARE(page.value().publishedCore, publishedCore);
    QCOMPARE(page.value().publishedRevision, revision);
    QCOMPARE(page.value().publishedBackground, std::optional<QByteArray>(backgroundHash));
    QCOMPARE(page.value().publishedSong, std::optional<QByteArray>(songHash));
    QCOMPARE(page.value().publishedAtMs, qint64(6'000));

    // A later draft replaces every draft field (a removed song really goes)
    // and leaves the published page alone.
    QVERIFY(store.pages->saveDraft(QByteArray("draft 2"), std::nullopt, songHash, QString(), 8'000)
                .hasValue());
    page = store.pages->localPage();
    QCOMPARE(page.value().draftCore, QByteArray("draft 2"));
    QVERIFY(!page.value().draftBackground);
    QCOMPARE(page.value().draftSong, std::optional<QByteArray>(songHash));
    QVERIFY(page.value().draftSongSource.isEmpty());
    QCOMPARE(page.value().draftUpdatedAtMs, qint64(8'000));
    QCOMPARE(page.value().publishedCore, publishedCore);

    QVERIFY(store.pages->clearDraft(9'000).hasValue());
    page = store.pages->localPage();
    QVERIFY(page.value().draftCore.isEmpty());
    QVERIFY(!page.value().draftSong);
    QCOMPARE(page.value().draftUpdatedAtMs, qint64(0));
    QCOMPARE(page.value().publishedRevision, revision);
    QCOMPARE(page.value().publishedSong, std::optional<QByteArray>(songHash));

    // Malformed input is refused and changes nothing.
    QCOMPARE(
        errorCode(store.pages->saveDraft(QByteArray(), std::nullopt, std::nullopt, QString(), 9)),
        invalidInput);
    QCOMPARE(
        errorCode(store.pages->saveDraft("x", QByteArray(31, 'h'), std::nullopt, QString(), 9)),
        invalidInput);
    QVERIFY(store.pages->localPage().value().draftCore.isEmpty());
}

void ProfilePageStoreTest::savePublishedClearsDraft()
{
    PageStore store;
    QVERIFY(store.open());
    const QByteArray songHash = blob('s').hash;
    QVERIFY(store.pages
                ->saveDraft("draft", blob('b').hash, songHash,
                            QStringLiteral(R"({"path":"a.wav"})"), 1'000)
                .hasValue());
    QVERIFY(store.pages->savePublished("page 7", 7, std::nullopt, songHash, 2'000).hasValue());

    auto page = store.pages->localPage().value();
    QVERIFY(page.draftCore.isEmpty());
    QVERIFY(!page.draftBackground);
    QVERIFY(!page.draftSong);
    QVERIFY(page.draftSongSource.isEmpty());
    QCOMPARE(page.draftUpdatedAtMs, qint64(0));
    QCOMPARE(page.publishedCore, QByteArray("page 7"));
    QCOMPARE(page.publishedRevision, qint64(7));
    QVERIFY(!page.publishedBackground);
    QCOMPARE(page.publishedSong, std::optional<QByteArray>(songHash));

    // A publish that is refused (not a newer revision) leaves both the
    // published page and the new draft exactly as they were.
    QVERIFY(
        store.pages->saveDraft("draft 2", std::nullopt, std::nullopt, QString(), 3'000).hasValue());
    for (const qint64 stale : {qint64(7), qint64(6)}) {
        const auto refused
            = store.pages->savePublished("stale", stale, std::nullopt, std::nullopt, 4'000);
        QVERIFY(!refused.hasValue());
        QCOMPARE(errorCode(refused), conflict);
    }
    QCOMPARE(errorCode(store.pages->savePublished("zero", 0, std::nullopt, std::nullopt, 4'000)),
             invalidInput);
    page = store.pages->localPage().value();
    QCOMPARE(page.draftCore, QByteArray("draft 2"));
    QCOMPARE(page.publishedCore, QByteArray("page 7"));
    QCOMPARE(page.publishedAtMs, qint64(2'000));

    QVERIFY(store.pages->savePublished("page 8", 8, std::nullopt, std::nullopt, 5'000).hasValue());
    page = store.pages->localPage().value();
    QVERIFY(page.draftCore.isEmpty());
    QCOMPARE(page.publishedCore, QByteArray("page 8"));
    QVERIFY(!page.publishedSong);
}

void ProfilePageStoreTest::localDraftMediaIsReferencedAtOnce()
{
    PageStore store;
    QVERIFY(store.open());
    const Blob replaced = blob('r');
    const Blob background = blob('b', 50'000);
    const Blob song = blob('s', 70'000);

    QVERIFY(store.importIntoDraft(backgroundKind, replaced, 1'000));
    QVERIFY(store.importIntoDraft(backgroundKind, background, 1'000));
    QVERIFY(store.importIntoDraft(songKind, song, 1'000));

    // Every slot points at its blob although no draft core was saved yet.
    auto page = store.pages->localPage().value();
    QVERIFY(page.draftCore.isEmpty());
    QCOMPARE(page.draftBackground, std::optional<QByteArray>(background.hash));
    QCOMPARE(page.draftSong, std::optional<QByteArray>(song.hash));

    // A collection with no grace at all, right away: only the blob the draft
    // no longer names goes.
    const auto collected = store.pages->collectGarbage(never, never);
    QVERIFY(collected.hasValue());
    QCOMPARE(collected.value(), 1);
    QCOMPARE(store.pages->localMedia(background.hash).value(),
             std::optional<QByteArray>(background.data));
    QCOMPARE(store.pages->localMedia(song.hash).value(), std::optional<QByteArray>(song.data));
    QVERIFY(!store.pages->localMedia(replaced.hash).value());
    QCOMPARE(store.blobCount(), qint64(2));

    // Saving the draft keeps the refs it names; the store never had a copy
    // of the replaced blob to hand out again.
    QVERIFY(
        store.pages->saveDraft("draft", background.hash, song.hash, QString(), 2'000).hasValue());
    QCOMPARE(store.pages->collectGarbage(never, never).value(), 0);
    QCOMPARE(store.blobCount(), qint64(2));
}

void ProfilePageStoreTest::localDraftMediaIsAllOrNothing()
{
    PageStore store;
    QVERIFY(store.open());
    const Blob picture = blob('p');

    // Malformed input never reaches the database.
    QCOMPARE(errorCode(store.pages->putLocalDraftMedia(3, picture.hash, picture.data, 1)),
             invalidInput);
    QCOMPARE(
        errorCode(store.pages->putLocalDraftMedia(backgroundKind, blob('q').hash, picture.data, 1)),
        invalidInput);
    QCOMPARE(store.blobCount(), qint64(0));

    // A page store for a profile with no local row cannot point a slot at the
    // blob (the local page references local_profiles), and the blob it had
    // already written in the same transaction is rolled back with it.
    SqlCipherProfilePageRepository orphan(store.database(), ProfileId::generate());
    const auto failed = orphan.putLocalDraftMedia(backgroundKind, picture.hash, picture.data, 1);
    QVERIFY(!failed.hasValue());
    QCOMPARE(errorCode(failed), conflict);
    QCOMPARE(store.blobCount(), qint64(0));

    // The same blob for the real profile goes in, and the repository is not
    // stuck in a half-open transaction.
    QVERIFY(store.importIntoDraft(backgroundKind, picture, 1));
    QCOMPARE(store.blobCount(), qint64(1));
}

void ProfilePageStoreTest::mediaIsStoredOnceByHash()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    const Blob picture = blob('p', 20'000);

    QVERIFY(store.importIntoDraft(backgroundKind, picture, 1'000));
    QVERIFY(store.receive(alice, backgroundKind, picture, 2'000));
    QVERIFY(store.receive(bob, backgroundKind, picture, 3'000));
    QVERIFY(store.receive(bob, backgroundKind, picture, 4'000));

    QCOMPARE(store.blobCount(), qint64(1));
    {
        RawDatabase raw(store.path());
        QCOMPARE(raw.integer("SELECT count(*) FROM contact_page_media"), qint64(2));
        // The first copy stays; later ones only add their sender's record.
        QCOMPARE(raw.integer("SELECT created_at_ms FROM profile_media"), qint64(1'000));
    }
    QCOMPARE(store.pages->localMedia(picture.hash).value(),
             std::optional<QByteArray>(picture.data));
    QCOMPARE(store.pages->contactMedia(alice, picture.hash, backgroundKind).value(),
             std::optional<QByteArray>(picture.data));
    QCOMPARE(store.pages->contactMedia(bob, picture.hash, backgroundKind).value(),
             std::optional<QByteArray>(picture.data));

    // Content addressing is enforced: bytes that are not what the hash names
    // are refused, so they can never stand in for the real blob.
    const Blob real = blob('r');
    const QByteArray forged(1'000, 'f');
    QCOMPARE(
        errorCode(store.pages->putContactMedia(alice, backgroundKind, real.hash, forged, 5'000)),
        invalidInput);
    QVERIFY(!store.pages->hasContactMedia(alice, real.hash, backgroundKind).value());
    QVERIFY(store.receive(bob, backgroundKind, real, 6'000));
    QCOMPARE(store.pages->contactMedia(bob, real.hash, backgroundKind).value(),
             std::optional<QByteArray>(real.data));
}

void ProfilePageStoreTest::contactMediaIsScopedByOwnerAndKind()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    const Blob picture = blob('p');

    QVERIFY(store.receive(alice, backgroundKind, picture, 1'000));

    QCOMPARE(store.pages->contactMedia(alice, picture.hash, backgroundKind).value(),
             std::optional<QByteArray>(picture.data));
    QVERIFY(store.pages->hasContactMedia(alice, picture.hash, backgroundKind).value());
    // Another contact cannot reach it, even knowing the hash...
    QVERIFY(!store.pages->contactMedia(bob, picture.hash, backgroundKind).value());
    QVERIFY(!store.pages->hasContactMedia(bob, picture.hash, backgroundKind).value());
    // ...the sender cannot have it used as another kind...
    QVERIFY(!store.pages->contactMedia(alice, picture.hash, songKind).value());
    QVERIFY(!store.pages->hasContactMedia(alice, picture.hash, songKind).value());
    // ...and it is not ours to publish.
    QVERIFY(!store.pages->localMedia(picture.hash).value());

    // Once Bob sends it himself, it is his too.
    QVERIFY(store.receive(bob, backgroundKind, picture, 2'000));
    QVERIFY(store.pages->hasContactMedia(bob, picture.hash, backgroundKind).value());

    QCOMPARE(errorCode(store.pages->contactMedia(alice, QByteArray(31, 'h'), backgroundKind)),
             invalidInput);
    QCOMPARE(errorCode(store.pages->hasContactMedia(alice, picture.hash, 0)), invalidInput);
    QCOMPARE(errorCode(store.pages->putContactMedia(alice, 3, picture.hash, picture.data, 1)),
             invalidInput);
}

void ProfilePageStoreTest::pendingContactMediaCountsOnlyUnnamedRows()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    const Blob background = blob('b');
    const Blob song = blob('s');
    const Blob stray = blob('x');

    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
    // Media that overtook its core is all pending.
    QVERIFY(store.receive(alice, backgroundKind, background, 1));
    QVERIFY(store.receive(alice, songKind, song, 2));
    QVERIFY(store.receive(alice, backgroundKind, stray, 3));
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 3);

    // The core adopts the two it names.
    QVERIFY(store.pages->storeContactPage(contactPage(alice, 1, "core", background.hash, song.hash))
                .value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 1);

    // Bob's rows are his own count, even for a blob Alice's core names.
    QVERIFY(store.receive(bob, backgroundKind, background, 4));
    QCOMPARE(store.pages->pendingContactMediaCount(bob).value(), 1);
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 1);

    // A core that names a different picture adopts the stray one and forgets
    // the picture it replaced rather than turning it pending; Bob's record of
    // the same blob is his own and stays.
    QVERIFY(store.pages->storeContactPage(contactPage(alice, 2, "core 2", stray.hash, song.hash))
                .value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
    QVERIFY(!store.pages->hasContactMedia(alice, background.hash, backgroundKind).value());
    QVERIFY(store.pages->hasContactMedia(bob, background.hash, backgroundKind).value());
    QCOMPARE(store.pages->pendingContactMediaCount(bob).value(), 1);
    QVERIFY(store.pages->storeContactPage(contactPage(alice, 3, "core 3")).value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
}

void ProfilePageStoreTest::contactPageStoresOnlyNewerRevisions()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const QByteArray background = blob('b').hash;
    const QByteArray song = blob('s').hash;

    QVERIFY(!store.pages->contactPage(alice).value());
    auto stored
        = store.pages->storeContactPage(contactPage(alice, 5, "core 5", background, song, 1'000));
    QVERIFY(stored.hasValue());
    QVERIFY(stored.value());
    QVERIFY(store.pages->markViewed(alice, 1'500).hasValue());

    auto page = store.pages->contactPage(alice).value();
    QVERIFY(page);
    QCOMPARE(page->account, alice);
    QCOMPARE(page->revision, qint64(5));
    QCOMPARE(page->core, QByteArray("core 5"));
    QCOMPARE(page->background, std::optional<QByteArray>(background));
    QCOMPARE(page->song, std::optional<QByteArray>(song));
    QCOMPARE(page->receivedAtMs, qint64(1'000));
    QCOMPARE(page->viewedAtMs, qint64(1'500));

    // The same revision again, or an older one, stores nothing.
    for (const qint64 revision : {qint64(5), qint64(4), qint64(0)}) {
        stored = store.pages->storeContactPage(
            contactPage(alice, revision, "replay", std::nullopt, std::nullopt, 2'000));
        QVERIFY(stored.hasValue());
        QVERIFY(!stored.value());
    }
    page = store.pages->contactPage(alice).value();
    QCOMPARE(page->core, QByteArray("core 5"));
    QCOMPARE(page->background, std::optional<QByteArray>(background));
    QCOMPARE(page->receivedAtMs, qint64(1'000));

    // A newer one replaces the page but not when it was last viewed.
    stored
        = store.pages->storeContactPage(contactPage(alice, 6, "core 6", std::nullopt, song, 3'000));
    QVERIFY(stored.value());
    page = store.pages->contactPage(alice).value();
    QCOMPARE(page->revision, qint64(6));
    QCOMPARE(page->core, QByteArray("core 6"));
    QVERIFY(!page->background);
    QCOMPARE(page->receivedAtMs, qint64(3'000));
    QCOMPARE(page->viewedAtMs, qint64(1'500));

    // A never-published page (revision 0) is still a page to store.
    const auto bob = AccountId::generate();
    QVERIFY(store.pages->storeContactPage(contactPage(bob, 0, "default")).value());
    QCOMPARE(store.pages->contactPage(bob).value()->revision, qint64(0));

    // Viewing someone without a page records nothing.
    const auto carol = AccountId::generate();
    QVERIFY(store.pages->markViewed(carol, 9).hasValue());
    QVERIFY(!store.pages->contactPage(carol).value());

    QCOMPARE(errorCode(store.pages->storeContactPage(contactPage(carol, -1, "x"))), invalidInput);
    QCOMPARE(errorCode(store.pages->storeContactPage(contactPage(carol, 1, QByteArray()))),
             invalidInput);
    QCOMPARE(
        errorCode(store.pages->storeContactPage(contactPage(carol, 1, "x", QByteArray(33, 'h')))),
        invalidInput);
}

void ProfilePageStoreTest::garbageCollectionKeepsEveryReferencedBlob()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    QVERIFY(store.addContact(alice, ContactState::Accepted));

    const Blob publishedBackground = blob('B');
    const Blob publishedSong = blob('S');
    const Blob draftBackground = blob('b');
    const Blob draftSong = blob('s');
    const Blob contactBackground = blob('c');
    const Blob contactSong = blob('d');
    const Blob contactPending = blob('e');
    const Blob oldUnreferenced = blob('o');
    const Blob freshUnreferenced = blob('f');

    // Old blobs (t = 1 000) the page still names, and one it stopped naming.
    QVERIFY(store.importIntoDraft(backgroundKind, publishedBackground, 1'000));
    QVERIFY(store.importIntoDraft(songKind, publishedSong, 1'000));
    QVERIFY(
        store.pages->savePublished("page", 1, publishedBackground.hash, publishedSong.hash, 1'000)
            .hasValue());
    QVERIFY(store.importIntoDraft(backgroundKind, oldUnreferenced, 1'000));
    QVERIFY(store.importIntoDraft(backgroundKind, draftBackground, 1'000));
    // A fresh song encode (t = 99 000) that a newer one already replaced.
    QVERIFY(store.importIntoDraft(songKind, freshUnreferenced, 99'000));
    QVERIFY(store.importIntoDraft(songKind, draftSong, 99'000));
    // Alice's page and its media, and a recent pending blob of hers.
    QVERIFY(store.receive(alice, backgroundKind, contactBackground, 1'000));
    QVERIFY(store.receive(alice, songKind, contactSong, 1'000));
    QVERIFY(store.pages
                ->storeContactPage(
                    contactPage(alice, 3, "core", contactBackground.hash, contactSong.hash, 1'000))
                .value());
    QVERIFY(store.receive(alice, backgroundKind, contactPending, 99'000));
    QCOMPARE(store.blobCount(), qint64(9));

    // now = 100 000; a day's pending time-to-live and a ten-minute grace.
    const qint64 now = 100'000;
    auto collected = store.pages->collectGarbage(now - 86'400'000, now - 10'000);
    QVERIFY(collected.hasValue());
    QCOMPARE(collected.value(), 1);
    QCOMPARE(store.blobCount(), qint64(8));
    QVERIFY(!store.pages->localMedia(oldUnreferenced.hash).value());
    for (const Blob *local : {&publishedBackground, &publishedSong, &draftBackground, &draftSong})
        QCOMPARE(store.pages->localMedia(local->hash).value(),
                 std::optional<QByteArray>(local->data));
    QVERIFY(store.pages->hasContactMedia(alice, contactBackground.hash, backgroundKind).value());
    QVERIFY(store.pages->hasContactMedia(alice, contactSong.hash, songKind).value());
    QVERIFY(store.pages->hasContactMedia(alice, contactPending.hash, backgroundKind).value());

    // Past its grace, the replaced encode goes too; nothing referenced does.
    collected = store.pages->collectGarbage(now - 86'400'000, never);
    QCOMPARE(collected.value(), 1);
    QCOMPARE(store.blobCount(), qint64(7));
    QCOMPARE(store.pages->collectGarbage(now - 86'400'000, never).value(), 0);
    QVERIFY(store.pages->hasContactMedia(alice, contactPending.hash, backgroundKind).value());
    QVERIFY(store.pages->contactPage(alice).value());

    // Publishing the draft empties every draft slot (NULLs in the reference
    // set must not shield anything) and frees the previous page's blobs.
    QVERIFY(store.pages->savePublished("page 2", 2, draftBackground.hash, draftSong.hash, now)
                .hasValue());
    QCOMPARE(store.pages->collectGarbage(now - 86'400'000, never).value(), 2);
    QVERIFY(!store.pages->localMedia(publishedBackground.hash).value());
    QVERIFY(!store.pages->localMedia(publishedSong.hash).value());
    QCOMPARE(store.pages->localMedia(draftBackground.hash).value(),
             std::optional<QByteArray>(draftBackground.data));
    QCOMPARE(store.pages->localMedia(draftSong.hash).value(),
             std::optional<QByteArray>(draftSong.data));
    QCOMPARE(store.blobCount(), qint64(5));
}

void ProfilePageStoreTest::collectionDropsRowsOfNonContacts()
{
    PageStore store;
    QVERIFY(store.open());
    const auto accepted = AccountId::generate();
    const auto incoming = AccountId::generate();
    const auto outgoing = AccountId::generate();
    const auto blocked = AccountId::generate();
    const auto stranger = AccountId::generate(); // no roster row at all
    QVERIFY(store.addContact(accepted, ContactState::Accepted));
    QVERIFY(store.addContact(incoming, ContactState::PendingIncoming));
    QVERIFY(store.addContact(outgoing, ContactState::PendingOutgoing));
    QVERIFY(store.addContact(blocked, ContactState::Blocked));

    const QList<AccountId> everyone{accepted, incoming, outgoing, blocked, stranger};
    char fill = 'a';
    QHash<AccountId, Blob> blobs;
    for (const AccountId &account : everyone) {
        const Blob own = blob(fill++);
        blobs.insert(account, own);
        QVERIFY(store.receive(account, backgroundKind, own, 1'000));
        QVERIFY(store.pages->storeContactPage(contactPage(account, 2, "core", own.hash)).value());
        PageDelivery delivery{account};
        delivery.sentRevision = 9;
        QVERIFY(store.pages->saveDelivery(delivery).hasValue());
        QVERIFY(store.pages->recordMediaSent(account, own.hash, 1'100).hasValue());
        PageRequestState request{account};
        request.unanswered = 3;
        QVERIFY(store.pages->saveRequestState(request).hasValue());
    }

    // Cut-offs of 0 expire nothing by age: only the roster rule acts.
    QVERIFY(store.pages->collectGarbage(0, 0).hasValue());
    const auto keeps = [&](const AccountId &account) {
        const Blob &own = blobs.value(account);
        return store.pages->contactPage(account).value().has_value()
            && store.pages->hasContactMedia(account, own.hash, backgroundKind).value()
            && store.pages->delivery(account).value().sentRevision == 9
            && store.pages->mediaSentAt(account, own.hash).value().has_value()
            && store.pages->requestState(account).value().unanswered == 3;
    };
    const auto forgot = [&](const AccountId &account) {
        const Blob &own = blobs.value(account);
        return !store.pages->contactPage(account).value().has_value()
            && !store.pages->hasContactMedia(account, own.hash, backgroundKind).value()
            && store.pages->delivery(account).value().sentRevision == -1
            && !store.pages->mediaSentAt(account, own.hash).value().has_value()
            && store.pages->requestState(account).value().unanswered == 0;
    };
    QVERIFY(keeps(accepted));
    QVERIFY(forgot(incoming));
    QVERIFY(forgot(outgoing));
    QVERIFY(forgot(blocked));
    QVERIFY(forgot(stranger));
    // Their blobs are now unreferenced; the grace was 0, so they stay until a
    // collection whose cut-off has passed them.
    QCOMPARE(store.blobCount(), qint64(5));
    QCOMPARE(store.pages->collectGarbage(0, never).value(), 4);
    QCOMPARE(store.blobCount(), qint64(1));

    // Blocking the last contact drops their page on the next sweep too.
    QVERIFY(store.contacts->block(accepted, 3'000).hasValue());
    QVERIFY(store.pages->dropPagesOfNonContacts().hasValue());
    QVERIFY(forgot(accepted));
}

void ProfilePageStoreTest::pendingMediaExpires()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    QVERIFY(store.addContact(alice, ContactState::Accepted));
    const Blob named = blob('n');
    const Blob stale = blob('s');
    const Blob recent = blob('r');

    QVERIFY(store.receive(alice, backgroundKind, named, 1'000));
    QVERIFY(store.receive(alice, songKind, stale, 1'000));
    QVERIFY(store.receive(alice, backgroundKind, recent, 50'000));
    QVERIFY(store.pages->storeContactPage(contactPage(alice, 1, "core", named.hash)).value());

    // TTL cut-off at 10 000: the old pending row goes (and its blob, which
    // nothing else references); the old but named one and the recent pending
    // one stay.
    auto collected = store.pages->collectGarbage(10'000, never);
    QVERIFY(collected.hasValue());
    QCOMPARE(collected.value(), 1);
    QVERIFY(!store.pages->hasContactMedia(alice, stale.hash, songKind).value());
    QVERIFY(store.pages->hasContactMedia(alice, named.hash, backgroundKind).value());
    QVERIFY(store.pages->hasContactMedia(alice, recent.hash, backgroundKind).value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 1);

    // Sending a pending blob again starts its time-to-live over.
    QVERIFY(store.receive(alice, backgroundKind, recent, 90'000));
    QCOMPARE(store.pages->collectGarbage(60'000, never).value(), 0);
    QVERIFY(store.pages->hasContactMedia(alice, recent.hash, backgroundKind).value());

    // A newer core adopts the recent blob and forgets the one it no longer
    // names at once, row and blob: nothing is left for the collection.
    QVERIFY(store.pages->storeContactPage(contactPage(alice, 2, "core 2", recent.hash)).value());
    QCOMPARE(store.blobCount(), qint64(1));
    QCOMPARE(store.pages->collectGarbage(60'000, never).value(), 0);
    QVERIFY(!store.pages->hasContactMedia(alice, named.hash, backgroundKind).value());
    QVERIFY(store.pages->hasContactMedia(alice, recent.hash, backgroundKind).value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
    QCOMPARE(store.blobCount(), qint64(1));
}

void ProfilePageStoreTest::localBlobGraceRunsFromWhenItIsLetGo()
{
    PageStore store;
    QVERIFY(store.open());
    constexpr qint64 minute = 60'000;
    constexpr qint64 grace = 10 * minute;
    // A collection at `nowMs` with a day's pending time-to-live and a
    // ten-minute grace, as ProfilePageSync runs it.
    const auto collectAt = [&](qint64 nowMs) {
        return store.pages->collectGarbage(nowMs - 86'400'000, nowMs - grace).value();
    };
    const auto createdAt = [&](const Blob &media) {
        RawDatabase raw(store.path());
        return raw.integer("SELECT created_at_ms FROM profile_media WHERE hex(sha256) = '"
                           + media.hash.toHex().toUpper() + "'");
    };
    const Blob first = blob('1');
    const Blob second = blob('2');
    const Blob song = blob('s');
    const Blob third = blob('3');

    // A picture imported long before it is replaced: the editor can undo the
    // replacement for the whole grace, counted from the replacement.
    QVERIFY(store.importIntoDraft(backgroundKind, first, 0));
    QVERIFY(store.importIntoDraft(backgroundKind, second, 15 * minute));
    QCOMPARE(createdAt(first), 15 * minute);
    QCOMPARE(collectAt(15 * minute + 1'000), 0);
    QCOMPARE(collectAt(25 * minute), 0);
    // Undo names it again (the autosave), and the page can use it.
    QVERIFY(store.pages->saveDraft("undo", first.hash, std::nullopt, QString(), 20 * minute)
                .hasValue());
    QCOMPARE(store.pages->localMedia(first.hash).value(),
             std::optional<QByteArray>(first.data));

    // Redo lets go of it again, and its grace starts over from then.
    QVERIFY(store.pages->saveDraft("redo", second.hash, std::nullopt, QString(), 40 * minute)
                .hasValue());
    QCOMPARE(createdAt(first), 40 * minute);
    QCOMPARE(createdAt(second), 20 * minute); // let go of by the undo, named again since
    QCOMPARE(collectAt(50 * minute), 0);
    QCOMPARE(collectAt(50 * minute + 1), 1);
    QVERIFY(!store.pages->localMedia(first.hash).value());
    QCOMPARE(store.blobCount(), qint64(1));

    // Publishing keeps naming the draft's blobs, and an autosave that keeps
    // naming them rewrites nothing: only a blob let go of is touched.
    QVERIFY(store.importIntoDraft(songKind, song, 60 * minute));
    QVERIFY(store.pages->savePublished("page 1", 1, second.hash, song.hash, 61 * minute)
                .hasValue());
    QVERIFY(store.pages->saveDraft("draft", second.hash, song.hash, QString(), 62 * minute)
                .hasValue());
    QCOMPARE(createdAt(second), 20 * minute);
    QCOMPARE(createdAt(song), 60 * minute);

    // A picture replaced in the draft while the published page still shows
    // it is not let go of; discarding the draft lets go of the new one.
    QVERIFY(store.importIntoDraft(backgroundKind, third, 63 * minute));
    QCOMPARE(createdAt(second), 20 * minute);
    QVERIFY(store.pages->clearDraft(90 * minute).hasValue());
    QCOMPARE(createdAt(third), 90 * minute);
    QCOMPARE(collectAt(100 * minute), 0);
    QCOMPARE(collectAt(100 * minute + 1), 1);
    QVERIFY(!store.pages->localMedia(third.hash).value());

    // A refused publish lets go of nothing; the next one lets go of the
    // blobs the page it replaces used.
    QCOMPARE(errorCode(store.pages->savePublished("stale", 1, std::nullopt, std::nullopt,
                                                  110 * minute)),
             conflict);
    QCOMPARE(createdAt(second), 20 * minute);
    QVERIFY(store.pages->savePublished("page 2", 2, std::nullopt, std::nullopt, 120 * minute)
                .hasValue());
    QCOMPARE(createdAt(second), 120 * minute);
    QCOMPARE(createdAt(song), 120 * minute);
    QCOMPARE(collectAt(130 * minute), 0);
    QCOMPARE(collectAt(130 * minute + 1), 2);
    QCOMPARE(store.blobCount(), qint64(0));

    // A clock that went back never moves the time back.
    const Blob later = blob('L');
    QVERIFY(store.importIntoDraft(backgroundKind, later, 200 * minute));
    QVERIFY(store.importIntoDraft(backgroundKind, first, 150 * minute));
    QCOMPARE(createdAt(later), 200 * minute);
}

void ProfilePageStoreTest::replacedCoreForgetsItsMedia()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    QVERIFY(store.addContact(alice, ContactState::Accepted));
    QVERIFY(store.addContact(bob, ContactState::Accepted));
    const auto rowsOf = [&](const AccountId &account) {
        RawDatabase raw(store.path());
        return raw.integer("SELECT count(*) FROM contact_page_media WHERE hex(account_id) = '"
                           + account.bytes().toHex().toUpper() + "'");
    };

    // A contact publishing revision after revision, each with a fresh
    // picture and song sent after its core (so stored, being named): only
    // the current two are ever held, none of them pending.
    char fill = 'A';
    for (qint64 revision = 1; revision <= 5; ++revision) {
        const Blob background = blob(fill++);
        const Blob song = blob(fill++);
        const qint64 at = revision * 1'000;
        QVERIFY(store.pages
                    ->storeContactPage(
                        contactPage(alice, revision, "core", background.hash, song.hash, at))
                    .value());
        QVERIFY(store.receive(alice, backgroundKind, background, at + 1));
        QVERIFY(store.receive(alice, songKind, song, at + 2));
        QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
        QCOMPARE(rowsOf(alice), qint64(2));
        QCOMPARE(store.blobCount(), qint64(2)); // the replaced ones went at once
        QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(2'000));
    }

    // Media that overtook a newer core was not named by the core being
    // replaced: it stays pending through the revision in between, and the
    // core it belongs to adopts it.
    const Blob early = blob('e');
    QVERIFY(store.receive(alice, backgroundKind, early, 6'000));
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 1);
    QVERIFY(store.pages
                ->storeContactPage(
                    contactPage(alice, 6, "core 6", blob('6').hash, std::nullopt, 6'100))
                .value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 1);
    QCOMPARE(rowsOf(alice), qint64(1));
    QVERIFY(store.pages
                ->storeContactPage(contactPage(alice, 7, "core 7", early.hash, std::nullopt, 7'000))
                .value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
    QCOMPARE(store.pages->contactMedia(alice, early.hash, backgroundKind).value(),
             std::optional<QByteArray>(early.data));

    // A replaced blob that another contact or our own page also uses keeps
    // its bytes for them; only this contact's record of it goes.
    const Blob shared = blob('h');
    const Blob ours = blob('o');
    QVERIFY(store.importIntoDraft(songKind, ours, 8'000));
    QVERIFY(store.pages
                ->storeContactPage(contactPage(alice, 8, "core 8", shared.hash, ours.hash, 8'000))
                .value());
    QVERIFY(store.receive(alice, backgroundKind, shared, 8'001));
    QVERIFY(store.receive(alice, songKind, ours, 8'002));
    QVERIFY(store.receive(bob, backgroundKind, shared, 8'003));
    QVERIFY(store.pages
                ->storeContactPage(
                    contactPage(alice, 9, "core 9", std::nullopt, std::nullopt, 9'000))
                .value());
    QCOMPARE(rowsOf(alice), qint64(0));
    QCOMPARE(store.pages->contactMedia(bob, shared.hash, backgroundKind).value(),
             std::optional<QByteArray>(shared.data));
    QCOMPARE(store.pages->localMedia(ours.hash).value(), std::optional<QByteArray>(ours.data));

    // A repeated or older core replaces nothing, so it forgets nothing.
    const Blob current = blob('c');
    QVERIFY(store.pages
                ->storeContactPage(
                    contactPage(alice, 10, "core 10", current.hash, std::nullopt, 10'000))
                .value());
    QVERIFY(store.receive(alice, backgroundKind, current, 10'001));
    for (const qint64 stale : {qint64(10), qint64(3)}) {
        const auto stored = store.pages->storeContactPage(
            contactPage(alice, stale, "replay", std::nullopt, std::nullopt, 11'000));
        QVERIFY(stored.hasValue());
        QVERIFY(!stored.value());
    }
    QCOMPARE(store.pages->contactMedia(alice, current.hash, backgroundKind).value(),
             std::optional<QByteArray>(current.data));
}

void ProfilePageStoreTest::deliveryDeviceAndCapabilityRoundTrip()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto firstDevice = DeviceId::generate();
    const auto secondDevice = DeviceId::generate();

    auto delivery = store.pages->delivery(alice);
    QVERIFY(delivery.hasValue());
    QCOMPARE(delivery.value().account, alice);
    QVERIFY(!delivery.value().device);
    QCOMPARE(delivery.value().sentRevision, qint64(-1));
    QCOMPARE(delivery.value().sentAtMs, qint64(0));
    QVERIFY(!delivery.value().pageCapable);
    QCOMPARE(delivery.value().lastAnswerAtMs, qint64(0));
    QCOMPARE(delivery.value().answerWindowStartMs, qint64(0));
    QCOMPARE(delivery.value().answersInWindow, 0);

    PageDelivery saved{alice, firstDevice, 1'700'000'000'123, 5'000, true, 6'000, 7'000, 3};
    QVERIFY(store.pages->saveDelivery(saved).hasValue());
    QVERIFY(store.reopen());
    delivery = store.pages->delivery(alice);
    QCOMPARE(delivery.value().device, std::optional<DeviceId>(firstDevice));
    QCOMPARE(delivery.value().sentRevision, qint64(1'700'000'000'123));
    QCOMPARE(delivery.value().sentAtMs, qint64(5'000));
    QVERIFY(delivery.value().pageCapable);
    QCOMPARE(delivery.value().lastAnswerAtMs, qint64(6'000));
    QCOMPARE(delivery.value().answerWindowStartMs, qint64(7'000));
    QCOMPARE(delivery.value().answersInWindow, 3);

    // The peer changed device: the pump starts that contact over.
    saved = PageDelivery{alice, secondDevice};
    QVERIFY(store.pages->saveDelivery(saved).hasValue());
    delivery = store.pages->delivery(alice);
    QCOMPARE(delivery.value().device, std::optional<DeviceId>(secondDevice));
    QCOMPARE(delivery.value().sentRevision, qint64(-1));
    QVERIFY(!delivery.value().pageCapable);
    QCOMPARE(delivery.value().answersInWindow, 0);

    saved.device.reset();
    saved.pageCapable = true;
    QVERIFY(store.pages->saveDelivery(saved).hasValue());
    delivery = store.pages->delivery(alice);
    QVERIFY(!delivery.value().device);
    QVERIFY(delivery.value().pageCapable);
    // Other contacts are untouched.
    QVERIFY(!store.pages->delivery(AccountId::generate()).value().pageCapable);
}

void ProfilePageStoreTest::forgetSentMediaForOneAccountAndExcept()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    const QByteArray first = blob('1').hash;
    const QByteArray second = blob('2').hash;
    const QByteArray third = blob('3').hash;

    QVERIFY(!store.pages->mediaSentAt(alice, first).value());
    QVERIFY(store.pages->recordMediaSent(alice, first, 100).hasValue());
    QVERIFY(store.pages->recordMediaSent(alice, second, 200).hasValue());
    QVERIFY(store.pages->recordMediaSent(bob, first, 300).hasValue());
    QVERIFY(store.pages->recordMediaSent(bob, third, 400).hasValue());
    QCOMPARE(store.pages->mediaSentAt(alice, first).value(), std::optional<qint64>(100));
    QVERIFY(store.pages->recordMediaSent(alice, first, 150).hasValue());
    QCOMPARE(store.pages->mediaSentAt(alice, first).value(), std::optional<qint64>(150));

    // One contact's device changed: only their records go.
    QVERIFY(store.pages->forgetSentMedia(alice).hasValue());
    QVERIFY(!store.pages->mediaSentAt(alice, first).value());
    QVERIFY(!store.pages->mediaSentAt(alice, second).value());
    QCOMPARE(store.pages->mediaSentAt(bob, first).value(), std::optional<qint64>(300));
    QCOMPARE(store.pages->mediaSentAt(bob, third).value(), std::optional<qint64>(400));

    // After a publish, records of blobs the new page does not use go, for
    // every contact.
    QVERIFY(store.pages->recordMediaSent(alice, second, 500).hasValue());
    QVERIFY(store.pages->recordMediaSent(alice, first, 600).hasValue());
    QVERIFY(store.pages->forgetSentMediaExcept({first, third}).hasValue());
    QCOMPARE(store.pages->mediaSentAt(alice, first).value(), std::optional<qint64>(600));
    QVERIFY(!store.pages->mediaSentAt(alice, second).value());
    QCOMPARE(store.pages->mediaSentAt(bob, first).value(), std::optional<qint64>(300));
    QCOMPARE(store.pages->mediaSentAt(bob, third).value(), std::optional<qint64>(400));
    QVERIFY(store.pages->forgetSentMediaExcept({third}).hasValue());
    QVERIFY(!store.pages->mediaSentAt(alice, first).value());
    QVERIFY(!store.pages->mediaSentAt(bob, first).value());
    QCOMPARE(store.pages->mediaSentAt(bob, third).value(), std::optional<qint64>(400));

    // A malformed hash is refused before anything is deleted.
    QCOMPARE(errorCode(store.pages->forgetSentMediaExcept({QByteArray()})), invalidInput);
    QCOMPARE(store.pages->mediaSentAt(bob, third).value(), std::optional<qint64>(400));
    QCOMPARE(errorCode(store.pages->recordMediaSent(bob, QByteArray(16, 'h'), 1)), invalidInput);

    // A page with no media keeps no records at all.
    QVERIFY(store.pages->forgetSentMediaExcept({}).hasValue());
    QVERIFY(!store.pages->mediaSentAt(bob, third).value());
}

void ProfilePageStoreTest::requestStateRoundTrip()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();

    auto state = store.pages->requestState(alice);
    QVERIFY(state.hasValue());
    QCOMPARE(state.value().account, alice);
    QCOMPARE(state.value().lastRequestAtMs, qint64(0));
    QCOMPARE(state.value().unanswered, 0);
    QCOMPARE(state.value().mediaRequestedRevision, qint64(-1));

    QVERIFY(store.pages->saveRequestState(PageRequestState{alice, 1'234, 4, 1'700'000'000'000})
                .hasValue());
    QVERIFY(store.reopen());
    state = store.pages->requestState(alice);
    QCOMPARE(state.value().lastRequestAtMs, qint64(1'234));
    QCOMPARE(state.value().unanswered, 4);
    QCOMPARE(state.value().mediaRequestedRevision, qint64(1'700'000'000'000));

    // An answer resets the back-off; the rest is overwritten as given.
    QVERIFY(store.pages->saveRequestState(PageRequestState{alice, 2'000, 0, -1}).hasValue());
    state = store.pages->requestState(alice);
    QCOMPARE(state.value().lastRequestAtMs, qint64(2'000));
    QCOMPARE(state.value().unanswered, 0);
    QCOMPARE(state.value().mediaRequestedRevision, qint64(-1));
    QCOMPARE(store.pages->requestState(AccountId::generate()).value().lastRequestAtMs, qint64(0));
}

void ProfilePageStoreTest::evictContactMediaKeepsTheCoreAndSharedBlobs()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    const Blob background = blob('b', 10'000); // Alice's alone
    const Blob song = blob('s', 20'000); // Bob sent it too
    const Blob pending = blob('p', 30'000); // Alice's, not named yet
    const Blob ours = blob('o', 40'000); // our own draft uses it

    QVERIFY(store.importIntoDraft(backgroundKind, ours, 100));
    QVERIFY(store.receive(alice, backgroundKind, background, 100));
    QVERIFY(store.receive(alice, songKind, song, 100));
    QVERIFY(store.receive(alice, backgroundKind, pending, 100));
    QVERIFY(store.receive(alice, backgroundKind, ours, 100));
    QVERIFY(store.receive(bob, songKind, song, 100));
    QVERIFY(store.pages
                ->storeContactPage(contactPage(alice, 4, "alice core", background.hash, song.hash))
                .value());
    QVERIFY(store.pages->markViewed(alice, 700).hasValue());
    QCOMPARE(store.blobCount(), qint64(4));

    QVERIFY(store.pages->evictContactMedia(alice).hasValue());

    // Alice's page itself is untouched: the viewer still knows what is missing.
    const auto page = store.pages->contactPage(alice).value();
    QVERIFY(page);
    QCOMPARE(page->revision, qint64(4));
    QCOMPARE(page->core, QByteArray("alice core"));
    QCOMPARE(page->background, std::optional<QByteArray>(background.hash));
    QCOMPARE(page->song, std::optional<QByteArray>(song.hash));
    QCOMPARE(page->viewedAtMs, qint64(700));
    // None of her blobs is hers any more...
    QVERIFY(!store.pages->hasContactMedia(alice, background.hash, backgroundKind).value());
    QVERIFY(!store.pages->hasContactMedia(alice, song.hash, songKind).value());
    QVERIFY(!store.pages->hasContactMedia(alice, pending.hash, backgroundKind).value());
    QVERIFY(!store.pages->hasContactMedia(alice, ours.hash, backgroundKind).value());
    QCOMPARE(store.pages->pendingContactMediaCount(alice).value(), 0);
    // ...the ones only she held are gone at once, although they are younger
    // than any grace period...
    QCOMPARE(store.blobCount(), qint64(2));
    // ...and the shared ones stay for their other holders.
    QCOMPARE(store.pages->contactMedia(bob, song.hash, songKind).value(),
             std::optional<QByteArray>(song.data));
    QCOMPARE(store.pages->localMedia(ours.hash).value(), std::optional<QByteArray>(ours.data));

    // A later answer puts her media back.
    QVERIFY(store.receive(alice, backgroundKind, background, 900));
    QCOMPARE(store.pages->contactMedia(alice, background.hash, backgroundKind).value(),
             std::optional<QByteArray>(background.data));
}

void ProfilePageStoreTest::leastRecentlyViewedOrdersByViewThenArrival()
{
    PageStore store;
    QVERIFY(store.open());
    const auto viewedLong = AccountId::generate();
    const auto viewedRecently = AccountId::generate();
    const auto neverViewedOld = AccountId::generate();
    const auto neverViewedNew = AccountId::generate();
    const auto mediaOnly = AccountId::generate(); // media overtook the core
    const auto coreOnly = AccountId::generate(); // nothing to evict
    const Blob picture = blob('p');

    const auto give = [&](const AccountId &account, qint64 receivedAtMs) {
        return store.receive(account, backgroundKind, picture, receivedAtMs)
            && store.pages
                   ->storeContactPage(
                       contactPage(account, 1, "core", picture.hash, std::nullopt, receivedAtMs))
                   .value();
    };
    QVERIFY(give(viewedLong, 100));
    QVERIFY(give(viewedRecently, 50));
    QVERIFY(give(neverViewedOld, 300));
    QVERIFY(give(neverViewedNew, 400));
    QVERIFY(store.receive(mediaOnly, backgroundKind, picture, 350));
    QVERIFY(store.pages->storeContactPage(contactPage(coreOnly, 1, "core")).value());
    QVERIFY(store.pages->markViewed(viewedLong, 1'000).hasValue());
    QVERIFY(store.pages->markViewed(viewedRecently, 2'000).hasValue());

    const auto order = store.pages->contactsLeastRecentlyViewed();
    QVERIFY(order.hasValue());
    QCOMPARE(order.value(),
             QVector<AccountId>(
                 {neverViewedOld, mediaOnly, neverViewedNew, viewedLong, viewedRecently}));
}

void ProfilePageStoreTest::receivedMediaBytesCountsOnlyContactMedia()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();
    const auto bob = AccountId::generate();
    const Blob ours = blob('o', 100);
    const Blob shared = blob('s', 1'000);
    const Blob bobs = blob('b', 3'000);
    const Blob pending = blob('p', 500);

    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(0));
    QVERIFY(store.importIntoDraft(backgroundKind, ours, 1));
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(0));

    QVERIFY(store.receive(alice, backgroundKind, shared, 1));
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(1'000));
    // A blob two contacts sent is stored, and counted, once.
    QVERIFY(store.receive(bob, backgroundKind, shared, 1));
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(1'000));
    // A blob our own page uses is ours, whoever else sent it.
    QVERIFY(store.receive(alice, backgroundKind, ours, 1));
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(1'000));
    QVERIFY(store.receive(bob, songKind, bobs, 1));
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(4'000));
    // Pending media takes space like any other.
    QVERIFY(store.receive(alice, backgroundKind, pending, 1));
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(4'500));

    QVERIFY(store.pages->evictContactMedia(bob).hasValue());
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(1'500));
    QVERIFY(store.pages->evictContactMedia(alice).hasValue());
    QCOMPARE(store.pages->receivedMediaBytes().value(), qint64(0));
    QCOMPARE(store.pages->localMedia(ours.hash).value(), std::optional<QByteArray>(ours.data));
}

void ProfilePageStoreTest::checkConstraintsRejectBadHashesAndOversizeBlobs()
{
    PageStore store;
    QVERIFY(store.open());
    const auto alice = AccountId::generate();

    // The repository refuses bad input itself...
    const QByteArray largest(maxBlobBytes, 'l');
    const QByteArray oversize(maxBlobBytes + 1, 'o');
    QVERIFY(store.pages->putContactMedia(alice, backgroundKind, sha256(largest), largest, 1)
                .hasValue());
    QCOMPARE(errorCode(store.pages->putContactMedia(alice, backgroundKind, sha256(oversize),
                                                    oversize, 1)),
             invalidInput);
    QCOMPARE(errorCode(store.pages->putContactMedia(alice, backgroundKind, QByteArray(31, 'h'),
                                                    largest, 1)),
             invalidInput);
    QCOMPARE(
        errorCode(store.pages->putLocalDraftMedia(songKind, sha256(QByteArray()), QByteArray(), 1)),
        invalidInput);
    QCOMPARE(errorCode(store.pages->savePublished("core", 1, std::nullopt, QByteArray(33, 's'), 1)),
             invalidInput);
    QCOMPARE(errorCode(store.pages->mediaSentAt(alice, QByteArray(31, 'h'))), invalidInput);
    QCOMPARE(errorCode(store.pages->localMedia(QByteArray())), invalidInput);
    store.close();

    // ...and the schema holds even for a writer that skips those checks.
    RawDatabase raw(store.path());
    QVERIFY(raw.isOpen());
    // Controls: well-formed rows go in, so each refusal below is the CHECK.
    QVERIFY(raw.exec("INSERT INTO profile_media VALUES(randomblob(32), 2, randomblob(229376), 1)"));
    QVERIFY(raw.exec("INSERT INTO profile_media VALUES(randomblob(32), 1, randomblob(1), 1)"));
    QVERIFY(
        raw.exec("INSERT INTO local_profile_page(profile_id, draft_background, published_song, "
                 "published_revision) VALUES(randomblob(16), randomblob(32), randomblob(32), 0)"));
    QVERIFY(raw.exec("INSERT INTO contact_pages VALUES(randomblob(16), 0, X'01', randomblob(32), "
                     "randomblob(32), 1, 0)"));
    QVERIFY(
        raw.exec("INSERT INTO contact_page_media VALUES(randomblob(16), randomblob(32), 2, 1)"));
    QVERIFY(raw.exec("INSERT INTO page_deliveries(account_id, device_id, page_capable) "
                     "VALUES(randomblob(16), randomblob(16), 1)"));
    QVERIFY(raw.exec("INSERT INTO page_delivery_media VALUES(randomblob(16), randomblob(32), 1)"));
    QVERIFY(raw.exec("INSERT INTO page_requests(account_id) VALUES(randomblob(16))"));

    const QList<QByteArray> refused{
        "INSERT INTO profile_media VALUES(randomblob(31), 1, randomblob(10), 1)",
        "INSERT INTO profile_media VALUES(randomblob(33), 1, randomblob(10), 1)",
        "INSERT INTO profile_media VALUES(randomblob(32), 3, randomblob(10), 1)",
        "INSERT INTO profile_media VALUES(randomblob(32), 0, randomblob(10), 1)",
        "INSERT INTO profile_media VALUES(randomblob(32), 1, zeroblob(0), 1)",
        "INSERT INTO profile_media VALUES(randomblob(32), 1, randomblob(229377), 1)",
        "INSERT INTO local_profile_page(profile_id, draft_background) "
        "VALUES(randomblob(16), randomblob(31))",
        "INSERT INTO local_profile_page(profile_id, draft_song) VALUES(randomblob(16), "
        "randomblob(33))",
        "INSERT INTO local_profile_page(profile_id, published_background) "
        "VALUES(randomblob(16), randomblob(16))",
        "INSERT INTO local_profile_page(profile_id, published_song) "
        "VALUES(randomblob(16), randomblob(64))",
        "INSERT INTO local_profile_page(profile_id, published_revision) VALUES(randomblob(16), -1)",
        "INSERT INTO contact_pages VALUES(randomblob(15), 0, X'01', NULL, NULL, 1, 0)",
        "INSERT INTO contact_pages VALUES(randomblob(16), -1, X'01', NULL, NULL, 1, 0)",
        "INSERT INTO contact_pages VALUES(randomblob(16), 0, X'01', randomblob(31), NULL, 1, 0)",
        "INSERT INTO contact_pages VALUES(randomblob(16), 0, X'01', NULL, randomblob(33), 1, 0)",
        "INSERT INTO contact_pages VALUES(randomblob(16), 0, NULL, NULL, NULL, 1, 0)",
        "INSERT INTO contact_page_media VALUES(randomblob(17), randomblob(32), 1, 1)",
        "INSERT INTO contact_page_media VALUES(randomblob(16), randomblob(31), 1, 1)",
        "INSERT INTO contact_page_media VALUES(randomblob(16), randomblob(32), 3, 1)",
        "INSERT INTO page_deliveries(account_id, device_id) VALUES(randomblob(16), randomblob(15))",
        "INSERT INTO page_deliveries(account_id, page_capable) VALUES(randomblob(16), 2)",
        "INSERT INTO page_deliveries(account_id) VALUES(randomblob(8))",
        "INSERT INTO page_delivery_media VALUES(randomblob(16), randomblob(31), 1)",
        "INSERT INTO page_delivery_media VALUES(randomblob(15), randomblob(32), 1)",
        "INSERT INTO page_requests(account_id) VALUES(randomblob(17))",
    };
    for (const QByteArray &sql : refused)
        QVERIFY2(raw.violatesConstraint(sql), sql.constData());
}

QTEST_GUILESS_MAIN(ProfilePageStoreTest)
#include "tst_profilepagestore.moc"
