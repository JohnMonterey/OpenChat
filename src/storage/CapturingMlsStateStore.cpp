#include "storage/CapturingMlsStateStore.h"

#include "storage/SqlCipherDatabase.h"

#include <utility>

namespace OpenChat {
namespace {

// Mirrors the bound enforced by SqlCipherDatabase::storeMlsState so a captured
// blob can never exceed what the durable UPSERT accepts.
constexpr qsizetype maximumMlsStateSize = qsizetype{8} * 1024 * 1024;

} // namespace

CapturingMlsStateStore::CapturingMlsStateStore(SqlCipherDatabase &database, ProfileId profileId)
    : m_database(database), m_profileId(std::move(profileId))
{
}

Result<QByteArray, MlsError> CapturingMlsStateStore::load()
{
    auto loaded = m_database.loadMlsState(m_profileId);
    if (!loaded.hasValue())
        return Result<QByteArray, MlsError>::failure(MlsError::Storage);
    return Result<QByteArray, MlsError>::success(std::move(loaded).value());
}

Result<void, MlsError> CapturingMlsStateStore::store(QByteArrayView state)
{
    if (state.size() > maximumMlsStateSize)
        return Result<void, MlsError>::failure(MlsError::Storage);
    m_pendingState = state.toByteArray();
    m_hasPending = true;
    return Result<void, MlsError>::success();
}

QByteArray CapturingMlsStateStore::takePendingState()
{
    if (!m_hasPending)
        return {};
    QByteArray captured = std::move(m_pendingState);
    m_pendingState.clear();
    m_hasPending = false;
    return captured;
}

} // namespace OpenChat
