#include "network/MlsSyncSession.h"

#include "storage/CapturingMlsStateStore.h"

#include <utility>

namespace OpenChat {

MlsSyncSession::MlsSyncSession(MlsClient &mls, CapturingMlsStateStore &stateStore)
    : m_mls(mls), m_stateStore(stateStore)
{
}

Result<QByteArray, MlsError> MlsSyncSession::encrypt(const ConversationId &conversation,
                                                     QByteArrayView plaintext)
{
    auto ciphertext = m_mls.encrypt(conversation, plaintext);
    if (!ciphertext)
        return Result<QByteArray, MlsError>::failure(ciphertext.error());
    return Result<QByteArray, MlsError>::success(std::move(ciphertext).value().bytes);
}

Result<SyncProcessOutcome, MlsError> MlsSyncSession::process(const ConversationId &conversation,
                                                            QByteArrayView mlsMessage)
{
    auto processed = m_mls.process(conversation, mlsMessage);
    if (!processed)
        return Result<SyncProcessOutcome, MlsError>::failure(processed.error());

    const MlsProcessResult &result = processed.value();
    SyncProcessOutcome outcome;
    // The MLS-authenticated sender credential is carried through unchanged; the
    // engine binds the message to it rather than to the relay envelope field.
    outcome.senderIdentity = result.senderIdentity;
    switch (result.kind) {
    case MlsProcessKind::Application:
        outcome.kind = SyncProcessOutcome::Kind::Application;
        outcome.applicationData = result.applicationData;
        break;
    case MlsProcessKind::Proposal:
    case MlsProcessKind::Commit:
        // Handshake traffic advances the ratchet but surfaces no plaintext row.
        outcome.kind = SyncProcessOutcome::Kind::Control;
        break;
    }
    return Result<SyncProcessOutcome, MlsError>::success(std::move(outcome));
}

QByteArray MlsSyncSession::takePendingState()
{
    return m_stateStore.takePendingState();
}

} // namespace OpenChat
