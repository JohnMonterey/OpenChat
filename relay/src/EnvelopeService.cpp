#include "EnvelopeService.h"

#include "RelayCrypto.h"
#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <limits>

namespace OpenChat::Relay {

EnvelopeService::EnvelopeService(PostgresStore &store)
    : EnvelopeService(store, Policy{})
{
}

EnvelopeService::EnvelopeService(PostgresStore &store, Policy policy)
    : m_store(store)
    , m_policy(policy)
{
}

Result<SubmitResult, RelayError>
EnvelopeService::submit(const AuthenticatedDevice &authenticatedDevice, QByteArrayView envelopeBytes)
{
    // Decode through the strict bounded canonical decoder. This validates the
    // schema, field sizes, canonical form, expiry range, and that the embedded
    // ciphertext hash matches the ciphertext — all without touching plaintext.
    const auto decoded = decodeEnvelope(envelopeBytes);
    if (!decoded.hasValue())
        return Result<SubmitResult, RelayError>::failure(RelayError::InvalidRequest);
    const CiphertextEnvelopeV1 &envelope = decoded.value();

    // The submitting device must be the envelope's sender.
    if (envelope.senderDeviceId != authenticatedDevice.deviceId
        || envelope.senderAccountId != authenticatedDevice.accountId)
        return Result<SubmitResult, RelayError>::failure(RelayError::Unauthorized);

    // Reject an envelope that has already expired. The codec bounds the
    // created/expiry range but not against the wall clock, so a replayed old
    // envelope would otherwise be stored and re-delivered until manual cleanup.
    if (envelope.expiresAtMs <= m_store.nowMs())
        return Result<SubmitResult, RelayError>::failure(RelayError::InvalidRequest);

    QSqlDatabase &db = m_store.database();

    // Load the sender's signing key and verify the envelope signature over the
    // exact canonical bytes (signature field cleared).
    QSqlQuery sender(db);
    sender.prepare(QStringLiteral(
        "SELECT signing_key, revoked_at_ms FROM devices WHERE device_id = ?"));
    sender.addBindValue(envelope.senderDeviceId.bytes());
    if (!sender.exec())
        return Result<SubmitResult, RelayError>::failure(RelayError::Internal);
    if (!sender.next())
        return Result<SubmitResult, RelayError>::failure(RelayError::NotFound);
    if (!sender.value(1).isNull())
        return Result<SubmitResult, RelayError>::failure(RelayError::Revoked);
    const QByteArray signingKey = sender.value(0).toByteArray();
    if (!verifyEd25519(signingKey, envelopeSigningInput(envelope), envelope.senderSignature))
        return Result<SubmitResult, RelayError>::failure(RelayError::Unauthorized);

    // The recipient device must exist and be active.
    QSqlQuery recipient(db);
    recipient.prepare(QStringLiteral("SELECT revoked_at_ms FROM devices WHERE device_id = ?"));
    recipient.addBindValue(envelope.recipientDeviceId.bytes());
    if (!recipient.exec())
        return Result<SubmitResult, RelayError>::failure(RelayError::Internal);
    if (!recipient.next())
        return Result<SubmitResult, RelayError>::failure(RelayError::NotFound);
    if (!recipient.value(0).isNull())
        return Result<SubmitResult, RelayError>::failure(RelayError::Revoked);

    const qint64 now = m_store.nowMs();
    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO inbox_messages (recipient_device_id, envelope_id, idempotency_key, "
        "sender_device_id, envelope, ciphertext_sha256, created_at_ms, accepted_at_ms, "
        "expires_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (recipient_device_id, sender_device_id, idempotency_key) DO NOTHING "
        "RETURNING server_sequence, accepted_at_ms"));
    insert.addBindValue(envelope.recipientDeviceId.bytes());
    insert.addBindValue(envelope.envelopeId.bytes());
    insert.addBindValue(envelope.idempotencyKey.bytes());
    insert.addBindValue(envelope.senderDeviceId.bytes());
    insert.addBindValue(envelopeBytes.toByteArray());
    insert.addBindValue(envelope.ciphertextSha256);
    insert.addBindValue(envelope.createdAtMs);
    insert.addBindValue(now);
    insert.addBindValue(envelope.expiresAtMs);
    if (!insert.exec()) {
        // A different envelope_id/idempotency_key combination that still hits
        // the (recipient, envelope_id) uniqueness is a conflicting resend.
        const bool conflict = insert.lastError().nativeErrorCode() == QLatin1String("23505");
        return Result<SubmitResult, RelayError>::failure(conflict ? RelayError::Conflict
                                                                  : RelayError::Internal);
    }

    if (insert.next()) {
        SubmitResult result;
        result.serverSequence = static_cast<quint64>(insert.value(0).toLongLong());
        result.acceptedAtMs = insert.value(1).toLongLong();
        result.duplicate = false;
        return Result<SubmitResult, RelayError>::success(result);
    }

    // No row inserted: an identical (recipient, idempotency_key) already exists.
    // Return the original acceptance idempotently.
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral(
        "SELECT server_sequence, accepted_at_ms FROM inbox_messages "
        "WHERE recipient_device_id = ? AND sender_device_id = ? AND idempotency_key = ?"));
    existing.addBindValue(envelope.recipientDeviceId.bytes());
    existing.addBindValue(envelope.senderDeviceId.bytes());
    existing.addBindValue(envelope.idempotencyKey.bytes());
    if (!existing.exec() || !existing.next())
        return Result<SubmitResult, RelayError>::failure(RelayError::Internal);
    SubmitResult result;
    result.serverSequence = static_cast<quint64>(existing.value(0).toLongLong());
    result.acceptedAtMs = existing.value(1).toLongLong();
    result.duplicate = true;
    return Result<SubmitResult, RelayError>::success(result);
}

Result<FetchResult, RelayError> EnvelopeService::fetchSince(const DeviceId &deviceId, quint64 since,
                                                            int limit)
{
    QSqlDatabase &db = m_store.database();

    QSqlQuery device(db);
    device.prepare(QStringLiteral("SELECT revoked_at_ms FROM devices WHERE device_id = ?"));
    device.addBindValue(deviceId.bytes());
    if (!device.exec())
        return Result<FetchResult, RelayError>::failure(RelayError::Internal);
    if (!device.next())
        return Result<FetchResult, RelayError>::failure(RelayError::NotFound);
    if (!device.value(0).isNull())
        return Result<FetchResult, RelayError>::failure(RelayError::Revoked);

    const int bounded = std::clamp(limit, 1, m_policy.maxFetch);
    // server_sequence is a positive BIGSERIAL; clamp the cursor so an oversized
    // unsigned watermark cannot wrap to a negative bind and match every row.
    const qlonglong sinceBound =
        since > static_cast<quint64>(std::numeric_limits<qlonglong>::max())
            ? std::numeric_limits<qlonglong>::max()
            : static_cast<qlonglong>(since);
    QSqlQuery rows(db);
    rows.prepare(QStringLiteral(
        "SELECT server_sequence, envelope FROM inbox_messages "
        "WHERE recipient_device_id = ? AND server_sequence > ? "
        "ORDER BY server_sequence LIMIT ?"));
    rows.addBindValue(deviceId.bytes());
    rows.addBindValue(sinceBound);
    rows.addBindValue(bounded);
    if (!rows.exec())
        return Result<FetchResult, RelayError>::failure(RelayError::Internal);

    FetchResult result;
    result.newWatermark = since;
    qint64 pageBytes = 0;
    while (rows.next()) {
        InboxItem item;
        item.serverSequence = static_cast<quint64>(rows.value(0).toLongLong());
        item.envelope = rows.value(1).toByteArray();
        // Bound the cumulative page size. Always include at least one item so a
        // single large envelope can still make progress; stop before a further
        // item would push the page past the cap, leaving the watermark at the
        // last included item so the client fetches the remainder next round.
        if (!result.items.isEmpty()
            && pageBytes + item.envelope.size() > m_policy.maxResponseBytes)
            break;
        pageBytes += item.envelope.size();
        result.newWatermark = std::max(result.newWatermark, item.serverSequence);
        result.items.append(item);
    }
    return Result<FetchResult, RelayError>::success(result);
}

Result<void, RelayError> EnvelopeService::acknowledge(const DeviceId &deviceId, quint64 watermark)
{
    QSqlDatabase &db = m_store.database();
    QSqlQuery upsert(db);
    upsert.prepare(QStringLiteral(
        "INSERT INTO device_watermarks (device_id, acked_sequence, updated_at_ms) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT (device_id) DO UPDATE SET "
        "acked_sequence = GREATEST(device_watermarks.acked_sequence, EXCLUDED.acked_sequence), "
        "updated_at_ms = EXCLUDED.updated_at_ms"));
    upsert.addBindValue(deviceId.bytes());
    upsert.addBindValue(static_cast<qlonglong>(watermark));
    upsert.addBindValue(m_store.nowMs());
    if (!upsert.exec())
        return Result<void, RelayError>::failure(RelayError::Internal);

    // Retention: an acknowledged envelope has been durably received, so drop it
    // (and any expired envelope for this device) instead of letting the inbox
    // grow without bound and re-serve the same rows on every catch-up.
    QSqlQuery prune(db);
    prune.prepare(QStringLiteral(
        "DELETE FROM inbox_messages WHERE recipient_device_id = ? "
        "AND (server_sequence <= ? OR expires_at_ms < ?)"));
    prune.addBindValue(deviceId.bytes());
    prune.addBindValue(static_cast<qlonglong>(watermark));
    prune.addBindValue(m_store.nowMs());
    if (!prune.exec())
        return Result<void, RelayError>::failure(RelayError::Internal);
    return Result<void, RelayError>::success();
}

quint64 EnvelopeService::watermarkFor(const DeviceId &deviceId)
{
    QSqlQuery row(m_store.database());
    row.prepare(QStringLiteral("SELECT acked_sequence FROM device_watermarks WHERE device_id = ?"));
    row.addBindValue(deviceId.bytes());
    if (!row.exec() || !row.next())
        return 0;
    return static_cast<quint64>(row.value(0).toLongLong());
}

} // namespace OpenChat::Relay
