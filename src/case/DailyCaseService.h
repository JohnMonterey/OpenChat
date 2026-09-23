#pragma once
#include <QDateTime>
#include <QString>
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
struct CaseReply {
    std::optional<CaseResult> result;
    bool newlyClaimed = false;
    QString error;
};
class DailyCaseService {
public:
    virtual ~DailyCaseService() = default;
    virtual CaseReply status(const QString &account) = 0;
    virtual CaseReply claim(const QString &account) = 0;
};
// Temporary local authority. It draws the reward it shows, but grants nothing:
// there is no inventory yet. An online adapter must perform eligibility +
// selection + consumption in one server transaction and return the existing
// claim for repeat requests, keyed by account/server day.
class LocalDailyCaseService final : public DailyCaseService {
public:
    explicit LocalDailyCaseService(QString directory = {});
    CaseReply status(const QString &account) override;
    CaseReply claim(const QString &account) override;
private:
    CaseReply transact(const QString &account, bool claim);
    QString m_directory;
};
}
