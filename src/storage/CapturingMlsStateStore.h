#pragma once

#include "core/Result.h"
#include "crypto/MlsClient.h" // MlsStateStore, MlsError
#include "domain/Identifiers.h"

#include <QByteArray>
#include <QByteArrayView>

namespace OpenChat {

class SqlCipherDatabase;

// An MlsStateStore that decouples advancing the in-memory MLS ratchet from
// persisting it. load() returns the last durably-committed blob (from
// local_mls_state), so a fresh MlsClient reconstructs from a consistent state.
// store() does NOT write the database: it only captures the new blob in memory
// and marks it pending. The engine then hands that blob to a SyncStore commit*
// so the ratchet becomes durable in the SAME transaction as the message /
// outbox / watermark it belongs to (finding F3). takePendingState() surrenders
// the captured blob to the engine and clears the pending marker.
class CapturingMlsStateStore final : public MlsStateStore
{
public:
    CapturingMlsStateStore(SqlCipherDatabase &database, ProfileId profileId);

    // Last durably-committed state for the profile (empty if none yet).
    [[nodiscard]] Result<QByteArray, MlsError> load() override;

    // Captures the new state in memory and marks it pending. Never touches disk.
    [[nodiscard]] Result<void, MlsError> store(QByteArrayView state) override;

    // Returns the captured blob and clears the pending marker. Returns an empty
    // QByteArray (and leaves nothing pending) when no state is pending.
    [[nodiscard]] QByteArray takePendingState();

    [[nodiscard]] bool hasPendingState() const noexcept { return m_hasPending; }

private:
    SqlCipherDatabase &m_database;
    ProfileId m_profileId;
    QByteArray m_pendingState;
    bool m_hasPending = false;
};

} // namespace OpenChat
