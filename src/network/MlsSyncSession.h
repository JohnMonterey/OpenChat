#pragma once

#include "core/Result.h"
#include "crypto/MlsClient.h" // MlsError
#include "domain/Identifiers.h"
#include "network/SyncEngine.h" // SyncMlsSession, SyncProcessOutcome

#include <QByteArray>
#include <QByteArrayView>

namespace OpenChat {

class CapturingMlsStateStore;

// Adapts the concrete MlsClient (plus its capturing state store) to the narrow
// SyncMlsSession surface the SyncEngine consumes.
//
// CONTRACT: the capture path only works when `mls` was constructed with the very
// same `stateStore` reference passed here, i.e.
// `MlsClient::create(identity, &stateStore)`. This adapter merely borrows both
// objects; it does not wire them together. encrypt()/process() advance the
// in-memory ratchet, and the MlsClient hands the new serialized state to
// `stateStore` (which captures it in memory instead of writing the DB), so
// takePendingState() can surrender that blob for the engine's atomic durable
// commit. Both referenced objects must outlive this adapter.
class MlsSyncSession final : public SyncMlsSession
{
public:
    MlsSyncSession(MlsClient &mls, CapturingMlsStateStore &stateStore);

    [[nodiscard]] Result<QByteArray, MlsError>
    encrypt(const ConversationId &conversation, QByteArrayView plaintext) override;
    [[nodiscard]] Result<SyncProcessOutcome, MlsError>
    process(const ConversationId &conversation, QByteArrayView mlsMessage) override;
    [[nodiscard]] QByteArray takePendingState() override;

private:
    MlsClient &m_mls;
    CapturingMlsStateStore &m_stateStore;
};

} // namespace OpenChat
