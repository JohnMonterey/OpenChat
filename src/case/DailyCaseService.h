#pragma once
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <optional>

namespace OpenChat {
// A case drops for every this much time OpenChat is running: two an hour.
// Time with the app closed earns nothing.
inline constexpr qint64 caseDropIntervalMs = 30 * 60 * 1000;

// One opened case. `rewardId` is a cosmetic catalogue id (see CosmeticCatalog)
// drawn by the authority with the tier odds, or "placeholder" for a claim saved
// before rewards existed. `seed` only arranges the reel; `caseKey` is the key
// the case had while it was on offer, so its belt can be shown again.
struct CaseResult {
    QString claimId;
    QString rewardId = QStringLiteral("placeholder");
    quint32 seed = 0;
    QString caseKey;
};
// `result` is the last case opened, if any. `drops` is how many cases wait to
// be opened, and `progressMs` how much running time already counts toward the
// next drop. `owned` is everything the account has unboxed. `caseKey` names the
// case on offer next; it seeds that case's belt, never its reward. All of them
// mean nothing when `error` is set.
struct CaseReply {
    std::optional<CaseResult> result;
    bool newlyClaimed = false;
    QString error;
    QStringList owned;
    QString caseKey;
    int drops = 0;
    qint64 progressMs = 0;
};
class DailyCaseService {
public:
    virtual ~DailyCaseService() = default;
    virtual CaseReply status(const QString &account) = 0;
    // Opens a waiting case; with none waiting, replays the last one opened.
    virtual CaseReply claim(const QString &account) = 0;
    // Counts `ms` of running time toward the next drop. One report credits at
    // most what the next drop still needs, so it adds one drop at most.
    virtual CaseReply accrue(const QString &account, qint64 ms) = 0;
    virtual qint64 dropIntervalMs() const { return caseDropIntervalMs; }
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
// Temporary local authority. It keeps each account's waiting drops and running
// time, draws the reward it shows and grants it into the account's
// LocalCosmeticInventory. It believes whatever running time it is told; an
// online adapter must count drops + perform selection + consumption + grant on
// the server, in one transaction per claim. (The DailyCase names predate drops.)
class LocalDailyCaseService final : public DailyCaseService {
public:
    explicit LocalDailyCaseService(QString directory = {}, qint64 dropIntervalMs = caseDropIntervalMs);
    CaseReply status(const QString &account) override;
    CaseReply claim(const QString &account) override;
    CaseReply accrue(const QString &account, qint64 ms) override;
    qint64 dropIntervalMs() const override { return m_dropIntervalMs; }
private:
    enum class Action { Status, Claim, Accrue };
    CaseReply transact(const QString &account, Action action, qint64 ms = 0);
    QString m_directory;
    qint64 m_dropIntervalMs;
    LocalCosmeticInventory m_inventory;
};
// Where the local authority keeps claims and collections unless told otherwise.
QString defaultDailyCaseDirectory();
}
