#include "storage/SqlCipherContactRepository.h"

#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

#include <optional>
#include <utility>

namespace OpenChat {
namespace {

using RepositorySql::Statement;

constexpr int pendingOutgoingInt = 0;
constexpr int pendingIncomingInt = 1;
constexpr int acceptedInt = 2;
constexpr int blockedInt = 3;

int toStateInt(ContactState state)
{
    switch (state) {
    case ContactState::PendingOutgoing:
        return pendingOutgoingInt;
    case ContactState::PendingIncoming:
        return pendingIncomingInt;
    case ContactState::Accepted:
        return acceptedInt;
    case ContactState::Blocked:
        return blockedInt;
    }
    return -1;
}

std::optional<ContactState> stateFromInt(int value)
{
    switch (value) {
    case pendingOutgoingInt:
        return ContactState::PendingOutgoing;
    case pendingIncomingInt:
        return ContactState::PendingIncoming;
    case acceptedInt:
        return ContactState::Accepted;
    case blockedInt:
        return ContactState::Blocked;
    default:
        return std::nullopt;
    }
}

RepositoryError internalError(const QString &code)
{
    return RepositorySql::error(RepositoryErrorCode::Internal, code);
}

Result<void, RepositoryError> okVoid()
{
    return Result<void, RepositoryError>::success();
}

Result<void, RepositoryError> failVoid(RepositoryError error)
{
    return Result<void, RepositoryError>::failure(std::move(error));
}

// Snapshot of an account's current row: whether the read succeeded, whether a
// row exists, and its raw state integer. Every mutation reads this first so it
// can branch on the existing state under the same locked connection.
struct ExistingContact final {
    bool queryOk = false;
    bool found = false;
    int state = 0;
};

ExistingContact readContactState(sqlite3 *database, const AccountId &accountId)
{
    Statement statement(database, "SELECT state FROM contacts WHERE account_id=?1");
    if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes()))
        return ExistingContact{false, false, 0};
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
        return ExistingContact{true, false, 0};
    if (step != SQLITE_ROW)
        return ExistingContact{false, false, 0};
    return ExistingContact{true, true, sqlite3_column_int(statement.get(), 0)};
}

Result<void, RepositoryError>
insertContact(sqlite3 *database, const ContactRecord &contact, ContactState state,
              const QString &code)
{
    Statement statement(database,
                        "INSERT INTO contacts(account_id, handle, display_name, state, "
                        "conversation_id, created_at_ms, updated_at_ms) "
                        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (!statement.isValid() || !statement.bindBlob(1, contact.accountId.bytes())
        || !statement.bindText(2, contact.handle) || !statement.bindText(3, contact.displayName)
        || !statement.bindInt(4, toStateInt(state)))
        return failVoid(internalError(code));
    const bool boundConversation = contact.conversationId
        ? statement.bindBlob(5, contact.conversationId->bytes())
        : statement.bindNull(5);
    if (!boundConversation || !statement.bindInt64(6, contact.createdAtMs)
        || !statement.bindInt64(7, contact.updatedAtMs)
        || sqlite3_step(statement.get()) != SQLITE_DONE)
        return failVoid(internalError(code));
    return okVoid();
}

// Refreshes the directory metadata and moves an existing row to a new state.
// created_at_ms and conversation_id are deliberately preserved.
Result<void, RepositoryError>
updateContactState(sqlite3 *database, const ContactRecord &contact, ContactState state,
                   const QString &code)
{
    Statement statement(database,
                        "UPDATE contacts SET handle=?2, display_name=?3, state=?4, "
                        "updated_at_ms=?5 WHERE account_id=?1");
    if (!statement.isValid() || !statement.bindBlob(1, contact.accountId.bytes())
        || !statement.bindText(2, contact.handle) || !statement.bindText(3, contact.displayName)
        || !statement.bindInt(4, toStateInt(state)) || !statement.bindInt64(5, contact.updatedAtMs)
        || sqlite3_step(statement.get()) != SQLITE_DONE)
        return failVoid(internalError(code));
    return okVoid();
}

Result<ContactRecord, RepositoryError> decodeContact(sqlite3_stmt *statement)
{
    using Ret = Result<ContactRecord, RepositoryError>;
    const auto accountId = AccountId::fromBytes(RepositorySql::blob(statement, 0));
    if (!accountId)
        return Ret::failure(RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                                 QStringLiteral("contact.decode.account")));
    const QString handle = RepositorySql::text(statement, 1);
    const QString displayName = RepositorySql::text(statement, 2);
    const auto state = stateFromInt(sqlite3_column_int(statement, 3));
    if (!state)
        return Ret::failure(RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                                 QStringLiteral("contact.decode.state")));
    std::optional<ConversationId> conversationId;
    if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
        const auto conversation = ConversationId::fromBytes(RepositorySql::blob(statement, 4));
        if (!conversation)
            return Ret::failure(RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                                     QStringLiteral("contact.decode.conversation")));
        conversationId = *conversation;
    }
    return Ret::success(ContactRecord{*accountId, handle, displayName, *state, conversationId,
                                      sqlite3_column_int64(statement, 5),
                                      sqlite3_column_int64(statement, 6)});
}

} // namespace

SqlCipherContactRepository::SqlCipherContactRepository(SqlCipherDatabase &database)
    : m_database(database)
{
}

Result<QVector<ContactRecord>, RepositoryError> SqlCipherContactRepository::contacts()
{
    return m_database.withConnection([&](sqlite3 *database) {
        using Ret = Result<QVector<ContactRecord>, RepositoryError>;
        Statement statement(database,
                            "SELECT account_id, handle, display_name, state, conversation_id, "
                            "created_at_ms, updated_at_ms FROM contacts "
                            "ORDER BY created_at_ms ASC, account_id ASC");
        if (!statement.isValid())
            return Ret::failure(internalError(QStringLiteral("contact.list.prepare")));
        QVector<ContactRecord> records;
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
            auto decoded = decodeContact(statement.get());
            if (!decoded.hasValue())
                return Ret::failure(decoded.error());
            records.push_back(std::move(decoded).value());
        }
        if (step != SQLITE_DONE)
            return Ret::failure(internalError(QStringLiteral("contact.list.read")));
        return Ret::success(std::move(records));
    });
}

Result<std::optional<ContactRecord>, RepositoryError>
SqlCipherContactRepository::find(const AccountId &accountId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        using Ret = Result<std::optional<ContactRecord>, RepositoryError>;
        Statement statement(database,
                            "SELECT account_id, handle, display_name, state, conversation_id, "
                            "created_at_ms, updated_at_ms FROM contacts WHERE account_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes()))
            return Ret::failure(internalError(QStringLiteral("contact.find.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Ret::success(std::optional<ContactRecord>{});
        if (step != SQLITE_ROW)
            return Ret::failure(internalError(QStringLiteral("contact.find.read")));
        auto decoded = decodeContact(statement.get());
        if (!decoded.hasValue())
            return Ret::failure(decoded.error());
        return Ret::success(std::optional<ContactRecord>(std::move(decoded).value()));
    });
}

Result<void, RepositoryError>
SqlCipherContactRepository::recordOutgoingRequest(const ContactRecord &contact)
{
    return m_database.withConnection([&](sqlite3 *database) {
        const auto existing = readContactState(database, contact.accountId);
        if (!existing.queryOk)
            return failVoid(internalError(QStringLiteral("contact.outgoing.read")));
        if (existing.found && existing.state == blockedInt)
            return failVoid(RepositorySql::error(RepositoryErrorCode::Conflict,
                                                 QStringLiteral("contact.outgoing.blocked")));
        if (existing.found)
            return updateContactState(database, contact, ContactState::PendingOutgoing,
                                      QStringLiteral("contact.outgoing.update"));
        return insertContact(database, contact, ContactState::PendingOutgoing,
                             QStringLiteral("contact.outgoing.insert"));
    });
}

Result<void, RepositoryError>
SqlCipherContactRepository::recordIncomingRequest(const ContactRecord &contact)
{
    return m_database.withConnection([&](sqlite3 *database) {
        const auto existing = readContactState(database, contact.accountId);
        if (!existing.queryOk)
            return failVoid(internalError(QStringLiteral("contact.incoming.read")));
        if (!existing.found)
            return insertContact(database, contact, ContactState::PendingIncoming,
                                 QStringLiteral("contact.incoming.insert"));
        // A Blocked peer is never resurrected, and an Accepted or PendingOutgoing
        // contact is never regressed by an untrusted incoming request. Only a
        // still-pending-incoming row refreshes its directory metadata; every
        // other existing state is left exactly as it was.
        if (existing.state == pendingIncomingInt)
            return updateContactState(database, contact, ContactState::PendingIncoming,
                                      QStringLiteral("contact.incoming.update"));
        return okVoid();
    });
}

Result<void, RepositoryError>
SqlCipherContactRepository::markAccepted(const AccountId &accountId,
                                         const ConversationId &conversationId, qint64 updatedAtMs)
{
    return m_database.withConnection([&](sqlite3 *database) {
        const auto existing = readContactState(database, accountId);
        if (!existing.queryOk)
            return failVoid(internalError(QStringLiteral("contact.accept.read")));
        if (!existing.found)
            return failVoid(RepositorySql::error(RepositoryErrorCode::NotFound,
                                                 QStringLiteral("contact.accept.missing")));
        if (existing.state == blockedInt)
            return failVoid(RepositorySql::error(RepositoryErrorCode::Conflict,
                                                 QStringLiteral("contact.accept.blocked")));
        Statement statement(database,
                            "UPDATE contacts SET state=?2, conversation_id=?3, updated_at_ms=?4 "
                            "WHERE account_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes())
            || !statement.bindInt(2, acceptedInt) || !statement.bindBlob(3, conversationId.bytes())
            || !statement.bindInt64(4, updatedAtMs) || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(internalError(QStringLiteral("contact.accept.update")));
        return okVoid();
    });
}

Result<void, RepositoryError>
SqlCipherContactRepository::block(const AccountId &accountId, qint64 updatedAtMs)
{
    return m_database.withConnection([&](sqlite3 *database) {
        const auto existing = readContactState(database, accountId);
        if (!existing.queryOk)
            return failVoid(internalError(QStringLiteral("contact.block.read")));
        if (existing.found) {
            Statement statement(database,
                                "UPDATE contacts SET state=?2, updated_at_ms=?3 "
                                "WHERE account_id=?1");
            if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes())
                || !statement.bindInt(2, blockedInt) || !statement.bindInt64(3, updatedAtMs)
                || sqlite3_step(statement.get()) != SQLITE_DONE)
                return failVoid(internalError(QStringLiteral("contact.block.update")));
            return okVoid();
        }
        Statement statement(database,
                            "INSERT INTO contacts(account_id, handle, display_name, state, "
                            "conversation_id, created_at_ms, updated_at_ms) "
                            "VALUES(?1, '', '', ?2, NULL, ?3, ?3)");
        if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes())
            || !statement.bindInt(2, blockedInt) || !statement.bindInt64(3, updatedAtMs)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(internalError(QStringLiteral("contact.block.insert")));
        return okVoid();
    });
}

Result<void, RepositoryError> SqlCipherContactRepository::unblock(const AccountId &accountId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        const auto existing = readContactState(database, accountId);
        if (!existing.queryOk)
            return failVoid(internalError(QStringLiteral("contact.unblock.read")));
        if (!existing.found)
            return failVoid(RepositorySql::error(RepositoryErrorCode::NotFound,
                                                 QStringLiteral("contact.unblock.missing")));
        if (existing.state != blockedInt)
            return failVoid(RepositorySql::error(RepositoryErrorCode::Conflict,
                                                 QStringLiteral("contact.unblock.notblocked")));
        Statement statement(database, "DELETE FROM contacts WHERE account_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(internalError(QStringLiteral("contact.unblock.delete")));
        return okVoid();
    });
}

Result<void, RepositoryError> SqlCipherContactRepository::remove(const AccountId &accountId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, "DELETE FROM contacts WHERE account_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, accountId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return failVoid(internalError(QStringLiteral("contact.remove.delete")));
        return okVoid();
    });
}

} // namespace OpenChat
