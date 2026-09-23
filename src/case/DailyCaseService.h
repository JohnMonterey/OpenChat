#pragma once
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <optional>

namespace OpenChat {
// A case can be opened again this long after the last one was.
inline constexpr int caseCooldownSeconds = 60 * 60;

// One claim. `rewardId` is a cosmetic catalogue id (see CosmeticCatalog) drawn
// by the authority with the tier odds, or "placeholder" for a claim saved
// before rewards existed. `seed` only arranges the reel. `nextAvailableAt` is
// when the next case can be opened: `caseCooldownSeconds` after this one.
struct CaseResult {
    QString claimId;
    QString rewardId = QStringLiteral("placeholder");
    quint32 seed = 0;
    QDateTime nextAvailableAt;
};
// `result` is the current claim while its cooldown runs, and empty once the
// next case is available. `owned` is everything the account has unboxed, the
// current reward included. `caseKey` names the case on offer, or the one just
// opened: its claim does not change it, and the next case gets a new one. It
// seeds the reel's belt, never the reward. Both mean nothing when `error` is set.
struct CaseReply {
    std::optional<CaseResult> result;
    bool newlyClaimed = false;
    QString error;
    QStringList owned;
    QString caseKey;
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
// eligibility (the cooldown, on server time) + selection + consumption + grant
// in one server transaction and return the existing claim for repeat requests.
// (The DailyCase names predate hourly cases.)
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
