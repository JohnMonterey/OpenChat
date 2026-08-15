#include "storage/SqlCipherSyncRepository.h"

#include "storage/RepositorySql.h"
#include "storage/SqlCipherDatabase.h"

#include <limits>

namespace OpenChat {
namespace {

using RepositorySql::Statement;

bool fitsSqlInteger(quint64 value)
{
    return value <= static_cast<quint64>(std::numeric_limits<qint64>::max());
}

RepositoryError internalError(const QString &code)
{
    return RepositorySql::error(RepositoryErrorCode::Internal, code);
}

} // namespace

SqlCipherSyncRepository::SqlCipherSyncRepository(SqlCipherDatabase &database)
    : m_database(database)
{
}

Result<SyncCursor, RepositoryError>
SqlCipherSyncRepository::cursor(const DeviceId &deviceId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "SELECT server_watermark, updated_at_ms "
                            "FROM device_sync_cursors WHERE device_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, deviceId.bytes()))
            return Result<SyncCursor, RepositoryError>::failure(
                internalError(QStringLiteral("sync.cursor.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
            return Result<SyncCursor, RepositoryError>::success(SyncCursor{deviceId, 0, 0});
        if (step != SQLITE_ROW || sqlite3_column_int64(statement.get(), 0) < 0)
            return Result<SyncCursor, RepositoryError>::failure(
                RepositorySql::error(RepositoryErrorCode::IntegrityFailure,
                                     QStringLiteral("sync.cursor.decode")));
        return Result<SyncCursor, RepositoryError>::success(
            SyncCursor{deviceId,
                       static_cast<quint64>(sqlite3_column_int64(statement.get(), 0)),
                       sqlite3_column_int64(statement.get(), 1)});
    });
}

Result<bool, RepositoryError>
SqlCipherSyncRepository::hasSeen(const EnvelopeId &envelopeId)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database, "SELECT 1 FROM replay_cache WHERE envelope_id=?1");
        if (!statement.isValid() || !statement.bindBlob(1, envelopeId.bytes()))
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("sync.replay.prepare")));
        const int step = sqlite3_step(statement.get());
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            return Result<bool, RepositoryError>::failure(
                internalError(QStringLiteral("sync.replay.read")));
        return Result<bool, RepositoryError>::success(step == SQLITE_ROW);
    });
}

Result<void, RepositoryError>
SqlCipherSyncRepository::recordSeen(const EnvelopeId &envelopeId,
                                    const DeviceId &senderDeviceId, qint64 receivedAtMs)
{
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT OR IGNORE INTO replay_cache("
                            "envelope_id, received_at_ms, sender_device_id) VALUES(?1, ?2, ?3)");
        if (!statement.isValid() || !statement.bindBlob(1, envelopeId.bytes())
            || !statement.bindInt64(2, receivedAtMs)
            || !statement.bindBlob(3, senderDeviceId.bytes())
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("sync.replay.insert")));
        return Result<void, RepositoryError>::success();
    });
}

Result<void, RepositoryError>
SqlCipherSyncRepository::advanceWatermark(const DeviceId &deviceId, quint64 watermark,
                                          qint64 updatedAtMs)
{
    if (!fitsSqlInteger(watermark))
        return Result<void, RepositoryError>::failure(
            RepositorySql::error(RepositoryErrorCode::InvalidInput,
                                 QStringLiteral("sync.watermark.range")));
    return m_database.withConnection([&](sqlite3 *database) {
        Statement statement(database,
                            "INSERT INTO device_sync_cursors("
                            "device_id, server_watermark, updated_at_ms) VALUES(?1, ?2, ?3) "
                            "ON CONFLICT(device_id) DO UPDATE SET "
                            "server_watermark=excluded.server_watermark, "
                            "updated_at_ms=excluded.updated_at_ms "
                            "WHERE excluded.server_watermark > device_sync_cursors.server_watermark");
        if (!statement.isValid() || !statement.bindBlob(1, deviceId.bytes())
            || !statement.bindInt64(2, static_cast<qint64>(watermark))
            || !statement.bindInt64(3, updatedAtMs)
            || sqlite3_step(statement.get()) != SQLITE_DONE)
            return Result<void, RepositoryError>::failure(
                internalError(QStringLiteral("sync.watermark.update")));
        return Result<void, RepositoryError>::success();
    });
}

} // namespace OpenChat
