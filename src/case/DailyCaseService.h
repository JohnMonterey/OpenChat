#pragma once
#include <QDateTime>
#include <QString>
#include <optional>

namespace OpenChat {
// Presentation metadata only. Future reward definitions can add name, icon,
// rarity, type and payload without changing the reel's positioning contract.
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
// Temporary local authority. No rewards are granted. An online adapter must
// perform eligibility + selection + consumption in one server transaction and
// return the existing claim for repeat requests, keyed by account/server day.
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
