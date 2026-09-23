#pragma once
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <optional>

namespace OpenChat {
// One claim. `rewardId` is a cosmetic catalogue id (see CosmeticCatalog) drawn
// by the authority with the tier odds, or "placeholder" for a claim saved
// before rewards existed. `seed` only arranges the reel.
struct CaseResult {
    QString claimId;
    QString rewardId = QStringLiteral("placeholder");
    quint32 seed = 0;
    QDateTime nextAvailableAt;
};
// `owned` is everything the account has unboxed, today's reward included,
// whether or not there is a claim today; it means nothing when `error` is set.
struct CaseReply {
    std::optional<CaseResult> result;
    bool newlyClaimed = false;
    QString error;
    QStringList owned;
};
class DailyCaseService {
public:
    virtual ~DailyCaseService() = default;
    virtual CaseReply status(const QString &account) = 0;
    virtual CaseReply claim(const QString &account) = 0;
};
// What each account has unboxed, kept by the local authority beside its
// claims: catalogue ids in the order they arrived, one atomic file per
// account. MOCK ONLY, like the claims: an online authority keeps the
// collection on the server and returns it with every reply.
class LocalCosmeticInventory {
public:
    explicit LocalCosmeticInventory(QString directory = {});
    // Nothing when the record exists but cannot be read.
    std::optional<QStringList> owned(const QString &account) const;
    // Adds the ids the account does not have yet; false if that could not be saved.
    bool grant(const QString &account, const QStringList &ids);
private:
    QString path(const QString &account) const;
    QString m_directory;
};
// Temporary local authority. It draws the reward it shows and grants it into
// the account's LocalCosmeticInventory. An online adapter must perform
// eligibility + selection + consumption + grant in one server transaction and
// return the existing claim for repeat requests, keyed by account/server day.
class LocalDailyCaseService final : public DailyCaseService {
public:
    explicit LocalDailyCaseService(QString directory = {});
    CaseReply status(const QString &account) override;
    CaseReply claim(const QString &account) override;
private:
    CaseReply transact(const QString &account, bool claim);
    QString m_directory;
    LocalCosmeticInventory m_inventory;
};
// Where the local authority keeps claims and collections unless told otherwise.
QString defaultDailyCaseDirectory();
}
