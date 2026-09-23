#include "DailyCaseService.h"
#include "cosmetics/CosmeticCatalog.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QTimeZone>

namespace OpenChat {
LocalDailyCaseService::LocalDailyCaseService(QString directory)
    : m_directory(directory.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/daily-case-preview"
          : std::move(directory)) {}
CaseReply LocalDailyCaseService::status(const QString &account) { return transact(account, false); }
CaseReply LocalDailyCaseService::claim(const QString &account) { return transact(account, true); }
CaseReply LocalDailyCaseService::transact(const QString &account, bool claim)
{
    if (account.isEmpty() || !QDir().mkpath(m_directory))
        return {{}, false, QStringLiteral("Could not access today's case. Please try again.")};
    const auto key = QCryptographicHash::hash(account.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString path = m_directory + '/' + QString::fromLatin1(key) + ".json";
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
        return {{}, false, QStringLiteral("Your case is being updated in another window. Please try again.")};
    const auto now = QDateTime::currentDateTimeUtc(); // MOCK ONLY; server time in online adapter.
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly))
            return {{}, false, QStringLiteral("Could not read today's case.")};
        const auto obj = QJsonDocument::fromJson(file.readAll()).object();
        const auto next = QDateTime::fromString(obj.value("next").toString(), Qt::ISODate);
        if (!next.isValid() || obj.value("claim").toString().isEmpty() || !obj.value("seed").isDouble())
            return {{}, false, QStringLiteral("The saved case could not be read.")};
        if (now < next)
            return {CaseResult{obj.value("claim").toString(),
                               obj.value("reward").toString(QStringLiteral("placeholder")),
                               quint32(obj.value("seed").toDouble()), next}, false, {}};
    }
    if (!claim)
        return {};
    // The reward is drawn here, by the authority, with the tier odds; the seed
    // below only arranges the reel. MOCK ONLY: the online adapter's server draws.
    auto *random = QRandomGenerator::global();
    const auto &reward = CosmeticCatalog::draw(random->bounded(quint32(CosmeticCatalog::totalWeight)),
                                               random->generate());
    CaseResult result{QUuid::createUuid().toString(QUuid::WithoutBraces), reward.id,
                      random->generate(),
                      QDateTime(now.date().addDays(1), QTime(0, 0), QTimeZone::UTC)};
    const auto bytes = QJsonDocument(QJsonObject{{"claim", result.claimId},
        {"reward", result.rewardId}, {"seed", double(result.seed)},
        {"next", result.nextAvailableAt.toString(Qt::ISODate)}}).toJson();
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        return {{}, false, QStringLiteral("Could not save today's case. Please try again.")};
    return {result, true, {}};
}
}
