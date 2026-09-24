#pragma once

#include "repositories/ProfilePageRepository.h"

namespace OpenChat {

class SqlCipherDatabase;

// ProfilePageRepository over the profile's SQLCipher database. The local page
// rows belong to `profileId`, which must name an existing local_profiles row
// (the local page references it). Contact rows are shared by the database.
class SqlCipherProfilePageRepository final : public ProfilePageRepository
{
public:
    SqlCipherProfilePageRepository(SqlCipherDatabase &database, ProfileId profileId);

    [[nodiscard]] Result<void, RepositoryError>
    putLocalDraftMedia(int kind, QByteArrayView sha256, QByteArrayView data, qint64 nowMs) override;
    [[nodiscard]] Result<std::optional<QByteArray>, RepositoryError>
    localMedia(QByteArrayView sha256) override;
    [[nodiscard]] Result<void, RepositoryError>
    putContactMedia(const AccountId &account, int kind, QByteArrayView sha256, QByteArrayView data,
                    qint64 nowMs) override;
    [[nodiscard]] Result<std::optional<QByteArray>, RepositoryError>
    contactMedia(const AccountId &account, QByteArrayView sha256, int kind) override;
    [[nodiscard]] Result<bool, RepositoryError>
    hasContactMedia(const AccountId &account, QByteArrayView sha256, int kind) override;
    [[nodiscard]] Result<int, RepositoryError>
    pendingContactMediaCount(const AccountId &account) override;
    [[nodiscard]] Result<int, RepositoryError>
    collectGarbage(qint64 pendingOlderThanMs, qint64 localBlobOlderThanMs) override;
    [[nodiscard]] Result<qint64, RepositoryError> receivedMediaBytes() override;
    [[nodiscard]] Result<void, RepositoryError>
    evictContactMedia(const AccountId &account) override;
    [[nodiscard]] Result<QVector<AccountId>, RepositoryError>
    contactsLeastRecentlyViewed() override;
    [[nodiscard]] Result<void, RepositoryError> dropPagesOfNonContacts() override;
    [[nodiscard]] Result<StoredLocalPage, RepositoryError> localPage() override;
    [[nodiscard]] Result<void, RepositoryError>
    saveDraft(QByteArrayView core, const std::optional<QByteArray> &background,
              const std::optional<QByteArray> &song, const QString &songSource,
              qint64 nowMs) override;
    [[nodiscard]] Result<void, RepositoryError> clearDraft(qint64 nowMs) override;
    [[nodiscard]] Result<void, RepositoryError>
    savePublished(QByteArrayView core, qint64 revision, const std::optional<QByteArray> &background,
                  const std::optional<QByteArray> &song, qint64 nowMs) override;
    [[nodiscard]] Result<std::optional<StoredContactPage>, RepositoryError>
    contactPage(const AccountId &account) override;
    [[nodiscard]] Result<bool, RepositoryError>
    storeContactPage(const StoredContactPage &page) override;
    [[nodiscard]] Result<void, RepositoryError>
    markViewed(const AccountId &account, qint64 nowMs) override;
    [[nodiscard]] Result<PageDelivery, RepositoryError> delivery(const AccountId &account) override;
    [[nodiscard]] Result<void, RepositoryError> saveDelivery(const PageDelivery &delivery) override;
    [[nodiscard]] Result<std::optional<qint64>, RepositoryError>
    mediaSentAt(const AccountId &account, QByteArrayView sha256) override;
    [[nodiscard]] Result<void, RepositoryError>
    recordMediaSent(const AccountId &account, QByteArrayView sha256, qint64 nowMs) override;
    [[nodiscard]] Result<void, RepositoryError> forgetSentMedia(const AccountId &account) override;
    [[nodiscard]] Result<void, RepositoryError>
    forgetSentMediaExcept(const QVector<QByteArray> &keep) override;
    [[nodiscard]] Result<PageRequestState, RepositoryError>
    requestState(const AccountId &account) override;
    [[nodiscard]] Result<void, RepositoryError>
    saveRequestState(const PageRequestState &state) override;

private:
    SqlCipherDatabase &m_database;
    ProfileId m_profileId;
};

} // namespace OpenChat
