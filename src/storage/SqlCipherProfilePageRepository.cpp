#include "storage/SqlCipherProfilePageRepository.h"

#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

#include <QCryptographicHash>

#include <utility>

namespace OpenChat {
namespace {

using RepositorySql::Statement;

constexpr qsizetype hashBytes = 32;
// profile_media's CHECK, and ProfilePageCodec's maxBackgroundImageBytes and
// maxSongBytes: the largest blob that fits one PageMedia message.
constexpr qsizetype maxMediaBytes = 224 * 1024;
constexpr int backgroundKind = 1;
constexpr int songKind = 2;
// Bounds forgetSentMediaExcept's statement; a page names at most two blobs.
constexpr qsizetype maxKeptHashes = 64;

RepositoryError error(RepositoryErrorCode code, const QString &diagnostic)
{
    return RepositorySql::error(code, diagnostic);
}

RepositoryError invalidInput(const QString &code)
{
    return error(RepositoryErrorCode::InvalidInput, code);
}

RepositoryError integrityFailure(const QString &code)
{
    return error(RepositoryErrorCode::IntegrityFailure, code);
}

// Translates the most recent SQLite result into a RepositoryError. Called
// before the transaction guard rolls back, so it reflects the statement that
// actually failed (a CHECK or foreign key violation reads as Conflict).
RepositoryError sqliteError(sqlite3 *database, const QString &code)
{
    RepositoryErrorCode mapped = RepositoryErrorCode::Internal;
    switch (sqlite3_extended_errcode(database) & 0xFF) {
    case SQLITE_FULL:
        mapped = RepositoryErrorCode::DiskFull;
        break;
    case SQLITE_CONSTRAINT:
        mapped = RepositoryErrorCode::Conflict;
        break;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        mapped = RepositoryErrorCode::IntegrityFailure;
        break;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        mapped = RepositoryErrorCode::Locked;
        break;
    case SQLITE_READONLY:
    case SQLITE_CANTOPEN:
    case SQLITE_IOERR:
    case SQLITE_PERM:
        mapped = RepositoryErrorCode::Unavailable;
        break;
    default:
        break;
    }
    return error(mapped, code);
}

Result<void, RepositoryError> okVoid()
{
    return Result<void, RepositoryError>::success();
}

Result<void, RepositoryError> failVoid(RepositoryError failure)
{
    return Result<void, RepositoryError>::failure(std::move(failure));
}

// BEGIN IMMEDIATE on construction; ROLLBACK on destruction unless committed,
// so every early return inside a multi-statement write leaves nothing behind.
class Transaction final
{
public:
    explicit Transaction(sqlite3 *database)
        : m_database(database)
        , m_open(RepositorySql::execute(database, "BEGIN IMMEDIATE;"))
    {
    }

    ~Transaction()
    {
        if (m_open)
            (void)RepositorySql::execute(m_database, "ROLLBACK;");
    }

    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }

    [[nodiscard]] bool commit()
    {
        if (!m_open || !RepositorySql::execute(m_database, "COMMIT;"))
            return false;
        m_open = false;
        return true;
    }

private:
    sqlite3 *m_database = nullptr;
    bool m_open = false;
};

bool isHash(QByteArrayView bytes)
{
    return bytes.size() == hashBytes;
}

bool isOptionalHash(const std::optional<QByteArray> &hash)
{
    return !hash || isHash(*hash);
}

bool isKind(int kind)
{
    return kind == backgroundKind || kind == songKind;
}

// A storable blob: within the size bounds and really the content `sha256`
// names. The store is content-addressed and a second copy of a hash is never
// written, so a mislabelled blob would stand in for the real one for good.
bool isBlobFor(QByteArrayView sha256, QByteArrayView data)
{
    return !data.isEmpty() && data.size() <= maxMediaBytes
           && QByteArrayView(QCryptographicHash::hash(data, QCryptographicHash::Sha256)) == sha256;
}

bool bindOptionalBlob(Statement &statement, int index, const std::optional<QByteArray> &value)
{
    return value ? statement.bindBlob(index, *value) : statement.bindNull(index);
}

// A nullable hash column: NULL reads as none; anything but 32 bytes is a
// corrupt row (the schema's CHECK forbids it).
bool readOptionalHash(sqlite3_stmt *statement, int column, std::optional<QByteArray> &hash)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        hash.reset();
        return true;
    }
    QByteArray bytes = RepositorySql::blob(statement, column);
    if (!isHash(bytes))
        return false;
    hash = std::move(bytes);
    return true;
}

// Stores a blob unless one with this hash exists already. Only the key
// conflict is skipped; a CHECK violation still fails (INSERT OR IGNORE would
// skip that silently too).
bool insertBlob(sqlite3 *database, int kind, QByteArrayView sha256, QByteArrayView data,
                qint64 nowMs)
{
    Statement statement(database,
                        "INSERT INTO profile_media(sha256, kind, data, created_at_ms) "
                        "VALUES(?1, ?2, ?3, ?4) ON CONFLICT(sha256) DO NOTHING");
    return statement.isValid() && statement.bindBlob(1, sha256) && statement.bindInt(2, kind)
           && statement.bindBlob(3, data) && statement.bindInt64(4, nowMs)
           && sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool run(sqlite3 *database, const char *sql)
{
    Statement statement(database, sql);
    return statement.isValid() && sqlite3_step(statement.get()) == SQLITE_DONE;
}

// The first five statements of the collection (§3.5): every page, media,
// delivery and request row of an account that is not an Accepted (2) contact.
// The caller owns the transaction.
bool deleteRowsOfNonContacts(sqlite3 *database)
{
    return run(database,
               "DELETE FROM contact_pages       WHERE account_id NOT IN "
               "(SELECT account_id FROM contacts WHERE state = 2)")
           && run(database,
                  "DELETE FROM contact_page_media  WHERE account_id NOT IN "
                  "(SELECT account_id FROM contacts WHERE state = 2)")
           && run(database,
                  "DELETE FROM page_deliveries     WHERE account_id NOT IN "
                  "(SELECT account_id FROM contacts WHERE state = 2)")
           && run(database,
                  "DELETE FROM page_delivery_media WHERE account_id NOT IN "
                  "(SELECT account_id FROM contacts WHERE state = 2)")
           && run(database,
                  "DELETE FROM page_requests       WHERE account_id NOT IN "
                  "(SELECT account_id FROM contacts WHERE state = 2)");
}

} // namespace

SqlCipherProfilePageRepository::SqlCipherProfilePageRepository(SqlCipherDatabase &database,
                                                               ProfileId profileId)
    : m_database(database)
    , m_profileId(std::move(profileId))
{
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::putLocalDraftMedia(int kind, QByteArrayView sha256,
                                                   QByteArrayView data, qint64 nowMs)
{
    if (!isKind(kind) || !isHash(sha256) || !isBlobFor(sha256, data))
        return failVoid(invalidInput(QStringLiteral("profilepage.localmedia.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Transaction transaction(database);
        if (!transaction.isOpen())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.localmedia.begin")));
        if (!insertBlob(database, kind, sha256, data, nowMs))
            return failVoid(sqliteError(database, QStringLiteral("profilepage.localmedia.blob")));
        Statement statement(database,
                            kind == backgroundKind
                                ? "INSERT INTO local_profile_page(profile_id, draft_background) "
                                  "VALUES(?1, ?2) ON CONFLICT(profile_id) DO UPDATE SET "
                                  "draft_background = excluded.draft_background"
                                : "INSERT INTO local_profile_page(profile_id, draft_song) "
                                  "VALUES(?1, ?2) ON CONFLICT(profile_id) DO UPDATE SET "
                                  "draft_song = excluded.draft_song");
        if (!statement.isValid() || !statement.bindBlob(1, m_profileId.bytes())
            || !statement.bindBlob(2, sha256) || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.localmedia.slot")));
        if (!transaction.commit())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.localmedia.commit")));
        return okVoid();
    });
}

Result<std::optional<QByteArray>, RepositoryError>
SqlCipherProfilePageRepository::localMedia(QByteArrayView sha256)
{
    using Ret = Result<std::optional<QByteArray>, RepositoryError>;
    if (!isHash(sha256))
        return Ret::failure(invalidInput(QStringLiteral("profilepage.localmedia.hash")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT m.data FROM profile_media m WHERE m.sha256 = ?1 AND EXISTS ("
                            "SELECT 1 FROM local_profile_page l WHERE l.profile_id = ?2 AND ("
                            "l.draft_background = ?1 OR l.draft_song = ?1 OR "
                            "l.published_background = ?1 OR l.published_song = ?1))");
        if (!statement.isValid() || !statement.bindBlob(1, sha256)
            || !statement.bindBlob(2, m_profileId.bytes()))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.localmedia.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.localmedia.read")));
        return Ret::success(RepositorySql::blob(statement.get(), 0));
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::putContactMedia(const AccountId &account, int kind,
                                                QByteArrayView sha256, QByteArrayView data,
                                                qint64 nowMs)
{
    if (!isKind(kind) || !isHash(sha256) || !isBlobFor(sha256, data))
        return failVoid(invalidInput(QStringLiteral("profilepage.contactmedia.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Transaction transaction(database);
        if (!transaction.isOpen())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.contactmedia.begin")));
        if (!insertBlob(database, kind, sha256, data, nowMs))
            return failVoid(sqliteError(database, QStringLiteral("profilepage.contactmedia.blob")));
        // A repeat of a blob this contact already sent refreshes its row: the
        // latest declaration of the kind counts, and a pending blob sent again
        // starts its time-to-live over.
        Statement statement(database,
                            "INSERT INTO contact_page_media(account_id, sha256, kind, received_at_ms) "
                            "VALUES(?1, ?2, ?3, ?4) ON CONFLICT(account_id, sha256) DO UPDATE SET "
                            "kind = excluded.kind, received_at_ms = excluded.received_at_ms");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || !statement.bindBlob(2, sha256) || !statement.bindInt(3, kind)
            || !statement.bindInt64(4, nowMs) || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.contactmedia.row")));
        if (!transaction.commit())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.contactmedia.commit")));
        return okVoid();
    });
}

Result<std::optional<QByteArray>, RepositoryError>
SqlCipherProfilePageRepository::contactMedia(const AccountId &account, QByteArrayView sha256,
                                             int kind)
{
    using Ret = Result<std::optional<QByteArray>, RepositoryError>;
    if (!isKind(kind) || !isHash(sha256))
        return Ret::failure(invalidInput(QStringLiteral("profilepage.contactmedia.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT m.data FROM contact_page_media c "
                            "JOIN profile_media m ON m.sha256 = c.sha256 "
                            "WHERE c.account_id = ?1 AND c.sha256 = ?2 AND c.kind = ?3");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || !statement.bindBlob(2, sha256) || !statement.bindInt(3, kind))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.contactmedia.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.contactmedia.read")));
        return Ret::success(RepositorySql::blob(statement.get(), 0));
    });
}

Result<bool, RepositoryError>
SqlCipherProfilePageRepository::hasContactMedia(const AccountId &account, QByteArrayView sha256,
                                                int kind)
{
    using Ret = Result<bool, RepositoryError>;
    if (!isKind(kind) || !isHash(sha256))
        return Ret::failure(invalidInput(QStringLiteral("profilepage.hasmedia.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        // Joined with the blob so "has" always means "can be read".
        Statement statement(database,
                            "SELECT 1 FROM contact_page_media c "
                            "JOIN profile_media m ON m.sha256 = c.sha256 "
                            "WHERE c.account_id = ?1 AND c.sha256 = ?2 AND c.kind = ?3");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || !statement.bindBlob(2, sha256) || !statement.bindInt(3, kind))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.hasmedia.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.hasmedia.read")));
        return Ret::success(step == SQLITE_ROW);
    });
}

Result<int, RepositoryError>
SqlCipherProfilePageRepository::pendingContactMediaCount(const AccountId &account)
{
    using Ret = Result<int, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        // The same "named by its core" test as the collection's pending-row
        // expiry, so what is counted here is exactly what would expire.
        Statement statement(database,
                            "SELECT count(*) FROM contact_page_media c WHERE c.account_id = ?1 "
                            "AND NOT EXISTS (SELECT 1 FROM contact_pages p "
                            "WHERE p.account_id = c.account_id "
                            "AND (p.background_sha256 = c.sha256 OR p.song_sha256 = c.sha256))");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || sqlite3_step(statement.get()) != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.pending.read")));
        return Ret::success(sqlite3_column_int(statement.get(), 0));
    });
}

Result<int, RepositoryError>
SqlCipherProfilePageRepository::collectGarbage(qint64 pendingOlderThanMs,
                                               qint64 localBlobOlderThanMs)
{
    using Ret = Result<int, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Transaction transaction(database);
        if (!transaction.isOpen())
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.gc.begin")));
        if (!deleteRowsOfNonContacts(database))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.gc.contacts")));

        // Pending rows (not named by that account's core) older than the TTL.
        Statement pending(database,
                          "DELETE FROM contact_page_media WHERE received_at_ms < ?1 AND NOT EXISTS ("
                          "SELECT 1 FROM contact_pages p "
                          "WHERE p.account_id = contact_page_media.account_id "
                          "AND (p.background_sha256 = contact_page_media.sha256 "
                          "OR p.song_sha256 = contact_page_media.sha256))");
        if (!pending.isValid() || !pending.bindInt64(1, pendingOlderThanMs)
            || sqlite3_step(pending.get()) != SQLITE_DONE)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.gc.pending")));

        // Blobs nobody references; a freshly stored blob is spared for the
        // grace period. The NULL filters matter: NOT IN over a set holding a
        // NULL is never true, which would keep every blob forever.
        Statement blobs(database,
                        "DELETE FROM profile_media WHERE created_at_ms < ?2 AND sha256 NOT IN ("
                        "SELECT draft_background FROM local_profile_page "
                        "WHERE draft_background IS NOT NULL "
                        "UNION SELECT draft_song FROM local_profile_page WHERE draft_song IS NOT NULL "
                        "UNION SELECT published_background FROM local_profile_page "
                        "WHERE published_background IS NOT NULL "
                        "UNION SELECT published_song FROM local_profile_page "
                        "WHERE published_song IS NOT NULL "
                        "UNION SELECT sha256 FROM contact_page_media)");
        if (!blobs.isValid() || !blobs.bindInt64(2, localBlobOlderThanMs)
            || sqlite3_step(blobs.get()) != SQLITE_DONE)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.gc.blobs")));
        const int deleted = sqlite3_changes(database);

        if (!transaction.commit())
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.gc.commit")));
        return Ret::success(deleted);
    });
}

Result<qint64, RepositoryError> SqlCipherProfilePageRepository::receivedMediaBytes()
{
    using Ret = Result<qint64, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        // Each blob counts once however many contacts sent it; blobs our own
        // page also uses are ours to keep and never evicted, so they are not
        // counted against the received-media cap.
        Statement statement(database,
                            "SELECT COALESCE(SUM(length(m.data)), 0) FROM profile_media m "
                            "WHERE EXISTS (SELECT 1 FROM contact_page_media c WHERE c.sha256 = m.sha256) "
                            "AND NOT EXISTS (SELECT 1 FROM local_profile_page l "
                            "WHERE l.draft_background = m.sha256 OR l.draft_song = m.sha256 "
                            "OR l.published_background = m.sha256 OR l.published_song = m.sha256)");
        if (!statement.isValid() || sqlite3_step(statement.get()) != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.received.read")));
        return Ret::success(sqlite3_column_int64(statement.get(), 0));
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::evictContactMedia(const AccountId &account)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Transaction transaction(database);
        if (!transaction.isOpen())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.evict.begin")));

        QVector<QByteArray> hashes;
        {
            Statement statement(database,
                                "SELECT sha256 FROM contact_page_media WHERE account_id = ?1");
            if (!statement.isValid() || !statement.bindBlob(1, account.bytes()))
                return failVoid(sqliteError(database, QStringLiteral("profilepage.evict.list")));
            int step = SQLITE_ROW;
            while ((step = sqlite3_step(statement.get())) == SQLITE_ROW)
                hashes.push_back(RepositorySql::blob(statement.get(), 0));
            if (step != SQLITE_DONE)
                return failVoid(sqliteError(database, QStringLiteral("profilepage.evict.list")));
        }

        Statement rows(database, "DELETE FROM contact_page_media WHERE account_id = ?1");
        if (!rows.isValid() || !rows.bindBlob(1, account.bytes())
            || sqlite3_step(rows.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.evict.rows")));

        // Only the blobs this account alone held go; one another contact also
        // sent, or our own page uses, stays for them.
        for (const QByteArray &hash : std::as_const(hashes)) {
            Statement blob(database,
                           "DELETE FROM profile_media WHERE sha256 = ?1 "
                           "AND NOT EXISTS (SELECT 1 FROM contact_page_media c WHERE c.sha256 = ?1) "
                           "AND NOT EXISTS (SELECT 1 FROM local_profile_page l "
                           "WHERE l.draft_background = ?1 OR l.draft_song = ?1 "
                           "OR l.published_background = ?1 OR l.published_song = ?1)");
            if (!blob.isValid() || !blob.bindBlob(1, hash)
                || sqlite3_step(blob.get()) != SQLITE_DONE)
                return failVoid(sqliteError(database, QStringLiteral("profilepage.evict.blob")));
        }

        if (!transaction.commit())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.evict.commit")));
        return okVoid();
    });
}

Result<QVector<AccountId>, RepositoryError>
SqlCipherProfilePageRepository::contactsLeastRecentlyViewed()
{
    using Ret = Result<QVector<AccountId>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        // An account whose media arrived before any core of theirs has no
        // page row: it was never viewed, and its oldest blob stands in for
        // the page's arrival. The account id makes the order total.
        Statement statement(database,
                            "SELECT a.account_id FROM (SELECT account_id, "
                            "MIN(received_at_ms) AS first_received FROM contact_page_media "
                            "GROUP BY account_id) a "
                            "LEFT JOIN contact_pages p ON p.account_id = a.account_id "
                            "ORDER BY COALESCE(p.viewed_at_ms, 0), "
                            "COALESCE(p.received_at_ms, a.first_received), a.account_id");
        if (!statement.isValid())
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.lru.prepare")));
        QVector<AccountId> accounts;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const auto account = AccountId::fromBytes(RepositorySql::blob(statement.get(), 0));
            if (!account)
                return Ret::failure(integrityFailure(QStringLiteral("profilepage.lru.account")));
            accounts.push_back(*account);
        }
        if (step != SQLITE_DONE)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.lru.read")));
        return Ret::success(std::move(accounts));
    });
}

Result<void, RepositoryError> SqlCipherProfilePageRepository::dropPagesOfNonContacts()
{
    return m_database.withConnection([&](sqlite3 *database) {
        Transaction transaction(database);
        if (!transaction.isOpen())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.drop.begin")));
        if (!deleteRowsOfNonContacts(database))
            return failVoid(sqliteError(database, QStringLiteral("profilepage.drop.delete")));
        if (!transaction.commit())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.drop.commit")));
        return okVoid();
    });
}

Result<StoredLocalPage, RepositoryError> SqlCipherProfilePageRepository::localPage()
{
    using Ret = Result<StoredLocalPage, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT draft_core, draft_background, draft_song, draft_song_source, "
                            "draft_updated_at_ms, published_core, published_revision, "
                            "published_background, published_song, published_at_ms "
                            "FROM local_profile_page WHERE profile_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, m_profileId.bytes()))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.local.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(StoredLocalPage{});
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.local.read")));
        sqlite3_stmt *row = statement.get();
        StoredLocalPage page;
        page.draftCore = RepositorySql::blob(row, 0);
        page.draftSongSource = RepositorySql::text(row, 3);
        page.draftUpdatedAtMs = sqlite3_column_int64(row, 4);
        page.publishedCore = RepositorySql::blob(row, 5);
        page.publishedRevision = sqlite3_column_int64(row, 6);
        page.publishedAtMs = sqlite3_column_int64(row, 9);
        if (!readOptionalHash(row, 1, page.draftBackground)
            || !readOptionalHash(row, 2, page.draftSong)
            || !readOptionalHash(row, 7, page.publishedBackground)
            || !readOptionalHash(row, 8, page.publishedSong))
            return Ret::failure(integrityFailure(QStringLiteral("profilepage.local.hash")));
        return Ret::success(std::move(page));
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::saveDraft(QByteArrayView core,
                                          const std::optional<QByteArray> &background,
                                          const std::optional<QByteArray> &song,
                                          const QString &songSource, qint64 nowMs)
{
    if (core.isEmpty() || !isOptionalHash(background) || !isOptionalHash(song))
        return failVoid(invalidInput(QStringLiteral("profilepage.draft.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO local_profile_page(profile_id, draft_core, draft_background, "
                            "draft_song, draft_song_source, draft_updated_at_ms) "
                            "VALUES(?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT(profile_id) DO UPDATE SET "
                            "draft_core = excluded.draft_core, "
                            "draft_background = excluded.draft_background, "
                            "draft_song = excluded.draft_song, "
                            "draft_song_source = excluded.draft_song_source, "
                            "draft_updated_at_ms = excluded.draft_updated_at_ms");
        if (!statement.isValid() || !statement.bindBlob(1, m_profileId.bytes())
            || !statement.bindBlob(2, core) || !bindOptionalBlob(statement, 3, background)
            || !bindOptionalBlob(statement, 4, song)
            || !(songSource.isEmpty() ? statement.bindNull(5) : statement.bindText(5, songSource))
            || !statement.bindInt64(6, nowMs) || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.draft.save")));
        return okVoid();
    });
}

Result<void, RepositoryError> SqlCipherProfilePageRepository::clearDraft()
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE local_profile_page SET draft_core = NULL, "
                            "draft_background = NULL, draft_song = NULL, draft_song_source = NULL, "
                            "draft_updated_at_ms = 0 WHERE profile_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, m_profileId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.draft.clear")));
        return okVoid();
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::savePublished(QByteArrayView core, qint64 revision,
                                              const std::optional<QByteArray> &background,
                                              const std::optional<QByteArray> &song, qint64 nowMs)
{
    // Revision 0 means "never published" on the wire, so a stored page is at
    // least revision 1.
    if (core.isEmpty() || revision < 1 || !isOptionalHash(background) || !isOptionalHash(song))
        return failVoid(invalidInput(QStringLiteral("profilepage.publish.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        // One statement, so the new page and the cleared draft commit together.
        // The WHERE refuses a revision that is not above the stored one.
        Statement statement(database,
                            "INSERT INTO local_profile_page(profile_id, published_core, "
                            "published_revision, published_background, published_song, "
                            "published_at_ms) VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
                            "ON CONFLICT(profile_id) DO UPDATE SET "
                            "published_core = excluded.published_core, "
                            "published_revision = excluded.published_revision, "
                            "published_background = excluded.published_background, "
                            "published_song = excluded.published_song, "
                            "published_at_ms = excluded.published_at_ms, "
                            "draft_core = NULL, draft_background = NULL, draft_song = NULL, "
                            "draft_song_source = NULL, draft_updated_at_ms = 0 "
                            "WHERE excluded.published_revision > local_profile_page.published_revision");
        if (!statement.isValid() || !statement.bindBlob(1, m_profileId.bytes())
            || !statement.bindBlob(2, core) || !statement.bindInt64(3, revision)
            || !bindOptionalBlob(statement, 4, background) || !bindOptionalBlob(statement, 5, song)
            || !statement.bindInt64(6, nowMs) || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.publish.save")));
        if (sqlite3_changes(database) == 0)
            return failVoid(error(RepositoryErrorCode::Conflict,
                                  QStringLiteral("profilepage.publish.revision")));
        return okVoid();
    });
}

Result<std::optional<StoredContactPage>, RepositoryError>
SqlCipherProfilePageRepository::contactPage(const AccountId &account)
{
    using Ret = Result<std::optional<StoredContactPage>, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT revision, core, background_sha256, song_sha256, "
                            "received_at_ms, viewed_at_ms FROM contact_pages WHERE account_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes()))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.contact.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.contact.read")));
        sqlite3_stmt *row = statement.get();
        StoredContactPage page{account};
        page.revision = sqlite3_column_int64(row, 0);
        page.core = RepositorySql::blob(row, 1);
        page.receivedAtMs = sqlite3_column_int64(row, 4);
        page.viewedAtMs = sqlite3_column_int64(row, 5);
        if (!readOptionalHash(row, 2, page.background) || !readOptionalHash(row, 3, page.song))
            return Ret::failure(integrityFailure(QStringLiteral("profilepage.contact.hash")));
        return Ret::success(std::move(page));
    });
}

Result<bool, RepositoryError>
SqlCipherProfilePageRepository::storeContactPage(const StoredContactPage &page)
{
    using Ret = Result<bool, RepositoryError>;
    if (page.revision < 0 || page.core.isEmpty() || !isOptionalHash(page.background)
        || !isOptionalHash(page.song))
        return Ret::failure(invalidInput(QStringLiteral("profilepage.contact.input")));
    return m_database.withConnection([&](sqlite3 *database) {
        // The WHERE makes "only newer revisions" part of the write itself, so
        // no read-then-write window exists between two arrivals.
        Statement statement(database,
                            "INSERT INTO contact_pages(account_id, revision, core, background_sha256, "
                            "song_sha256, received_at_ms, viewed_at_ms) "
                            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) ON CONFLICT(account_id) DO UPDATE SET "
                            "revision = excluded.revision, core = excluded.core, "
                            "background_sha256 = excluded.background_sha256, "
                            "song_sha256 = excluded.song_sha256, "
                            "received_at_ms = excluded.received_at_ms "
                            "WHERE excluded.revision > contact_pages.revision");
        if (!statement.isValid() || !statement.bindBlob(1, page.account.bytes())
            || !statement.bindInt64(2, page.revision) || !statement.bindBlob(3, page.core)
            || !bindOptionalBlob(statement, 4, page.background)
            || !bindOptionalBlob(statement, 5, page.song)
            || !statement.bindInt64(6, page.receivedAtMs) || !statement.bindInt64(7, page.viewedAtMs)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.contact.store")));
        return Ret::success(sqlite3_changes(database) > 0);
    });
}

Result<void, RepositoryError> SqlCipherProfilePageRepository::markViewed(const AccountId &account,
                                                                         qint64 nowMs)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "UPDATE contact_pages SET viewed_at_ms = ?2 WHERE account_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || !statement.bindInt64(2, nowMs) || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.viewed.update")));
        return okVoid();
    });
}

Result<PageDelivery, RepositoryError>
SqlCipherProfilePageRepository::delivery(const AccountId &account)
{
    using Ret = Result<PageDelivery, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT device_id, sent_revision, sent_at_ms, page_capable, "
                            "last_answer_at_ms, answer_window_start_ms, answers_in_window "
                            "FROM page_deliveries WHERE account_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes()))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.delivery.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(PageDelivery{account});
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.delivery.read")));
        sqlite3_stmt *row = statement.get();
        PageDelivery delivery{account};
        if (sqlite3_column_type(row, 0) != SQLITE_NULL) {
            const auto device = DeviceId::fromBytes(RepositorySql::blob(row, 0));
            if (!device)
                return Ret::failure(integrityFailure(QStringLiteral("profilepage.delivery.device")));
            delivery.device = *device;
        }
        delivery.sentRevision = sqlite3_column_int64(row, 1);
        delivery.sentAtMs = sqlite3_column_int64(row, 2);
        delivery.pageCapable = sqlite3_column_int(row, 3) != 0;
        delivery.lastAnswerAtMs = sqlite3_column_int64(row, 4);
        delivery.answerWindowStartMs = sqlite3_column_int64(row, 5);
        delivery.answersInWindow = sqlite3_column_int(row, 6);
        return Ret::success(std::move(delivery));
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::saveDelivery(const PageDelivery &delivery)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO page_deliveries(account_id, device_id, sent_revision, "
                            "sent_at_ms, page_capable, last_answer_at_ms, answer_window_start_ms, "
                            "answers_in_window) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
                            "ON CONFLICT(account_id) DO UPDATE SET device_id = excluded.device_id, "
                            "sent_revision = excluded.sent_revision, "
                            "sent_at_ms = excluded.sent_at_ms, "
                            "page_capable = excluded.page_capable, "
                            "last_answer_at_ms = excluded.last_answer_at_ms, "
                            "answer_window_start_ms = excluded.answer_window_start_ms, "
                            "answers_in_window = excluded.answers_in_window");
        if (!statement.isValid() || !statement.bindBlob(1, delivery.account.bytes())
            || !(delivery.device ? statement.bindBlob(2, delivery.device->bytes())
                                 : statement.bindNull(2))
            || !statement.bindInt64(3, delivery.sentRevision)
            || !statement.bindInt64(4, delivery.sentAtMs)
            || !statement.bindInt(5, delivery.pageCapable ? 1 : 0)
            || !statement.bindInt64(6, delivery.lastAnswerAtMs)
            || !statement.bindInt64(7, delivery.answerWindowStartMs)
            || !statement.bindInt(8, delivery.answersInWindow)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.delivery.save")));
        return okVoid();
    });
}

Result<std::optional<qint64>, RepositoryError>
SqlCipherProfilePageRepository::mediaSentAt(const AccountId &account, QByteArrayView sha256)
{
    using Ret = Result<std::optional<qint64>, RepositoryError>;
    if (!isHash(sha256))
        return Ret::failure(invalidInput(QStringLiteral("profilepage.sent.hash")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT sent_at_ms FROM page_delivery_media "
                            "WHERE account_id = ?1 AND sha256 = ?2");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || !statement.bindBlob(2, sha256))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.sent.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::nullopt);
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.sent.read")));
        return Ret::success(sqlite3_column_int64(statement.get(), 0));
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::recordMediaSent(const AccountId &account, QByteArrayView sha256,
                                                qint64 nowMs)
{
    if (!isHash(sha256))
        return failVoid(invalidInput(QStringLiteral("profilepage.sent.hash")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO page_delivery_media(account_id, sha256, sent_at_ms) "
                            "VALUES(?1, ?2, ?3) ON CONFLICT(account_id, sha256) DO UPDATE SET "
                            "sent_at_ms = excluded.sent_at_ms");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || !statement.bindBlob(2, sha256) || !statement.bindInt64(3, nowMs)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.sent.record")));
        return okVoid();
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::forgetSentMedia(const AccountId &account)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, "DELETE FROM page_delivery_media WHERE account_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.sent.forget")));
        return okVoid();
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::forgetSentMediaExcept(const QVector<QByteArray> &keep)
{
    if (keep.size() > maxKeptHashes)
        return failVoid(invalidInput(QStringLiteral("profilepage.sent.keepcount")));
    for (const QByteArray &hash : keep) {
        if (!isHash(hash))
            return failVoid(invalidInput(QStringLiteral("profilepage.sent.keephash")));
    }
    // One placeholder per kept hash; with nothing to keep, every record goes.
    QByteArray sql("DELETE FROM page_delivery_media");
    if (!keep.isEmpty()) {
        sql += " WHERE sha256 NOT IN (";
        for (qsizetype index = 0; index < keep.size(); ++index) {
            if (index > 0)
                sql += ", ";
            sql += '?' + QByteArray::number(index + 1);
        }
        sql += ')';
    }
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, sql.constData());
        if (!statement.isValid())
            return failVoid(sqliteError(database, QStringLiteral("profilepage.sent.except")));
        for (qsizetype index = 0; index < keep.size(); ++index) {
            if (!statement.bindBlob(static_cast<int>(index + 1), keep.at(index)))
                return failVoid(sqliteError(database, QStringLiteral("profilepage.sent.except")));
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.sent.except")));
        return okVoid();
    });
}

Result<PageRequestState, RepositoryError>
SqlCipherProfilePageRepository::requestState(const AccountId &account)
{
    using Ret = Result<PageRequestState, RepositoryError>;
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT last_request_at_ms, unanswered, media_requested_revision "
                            "FROM page_requests WHERE account_id = ?1");
        if (!statement.isValid() || !statement.bindBlob(1, account.bytes()))
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.request.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(PageRequestState{account});
        if (step != SQLITE_ROW)
            return Ret::failure(sqliteError(database, QStringLiteral("profilepage.request.read")));
        PageRequestState state{account};
        state.lastRequestAtMs = sqlite3_column_int64(statement.get(), 0);
        state.unanswered = sqlite3_column_int(statement.get(), 1);
        state.mediaRequestedRevision = sqlite3_column_int64(statement.get(), 2);
        return Ret::success(std::move(state));
    });
}

Result<void, RepositoryError>
SqlCipherProfilePageRepository::saveRequestState(const PageRequestState &state)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO page_requests(account_id, last_request_at_ms, unanswered, "
                            "media_requested_revision) VALUES(?1, ?2, ?3, ?4) "
                            "ON CONFLICT(account_id) DO UPDATE SET "
                            "last_request_at_ms = excluded.last_request_at_ms, "
                            "unanswered = excluded.unanswered, "
                            "media_requested_revision = excluded.media_requested_revision");
        if (!statement.isValid() || !statement.bindBlob(1, state.account.bytes())
            || !statement.bindInt64(2, state.lastRequestAtMs)
            || !statement.bindInt(3, state.unanswered)
            || !statement.bindInt64(4, state.mediaRequestedRevision)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(sqliteError(database, QStringLiteral("profilepage.request.save")));
        return okVoid();
    });
}

} // namespace OpenChat
