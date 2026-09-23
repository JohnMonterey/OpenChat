#include "DailyCaseService.h"
#include "cosmetics/CosmeticCatalog.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QTimeZone>

namespace OpenChat {
namespace {
QString accountFileStem(const QString &directory, const QString &account)
{
    const auto key = QCryptographicHash::hash(account.toUtf8(), QCryptographicHash::Sha256).toHex();
    return directory + '/' + QString::fromLatin1(key);
}
}

QString defaultDailyCaseDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/daily-case-preview";
}

LocalCosmeticInventory::LocalCosmeticInventory(QString directory)
    : m_directory(directory.isEmpty() ? defaultDailyCaseDirectory() : std::move(directory)) {}
QString LocalCosmeticInventory::path(const QString &account) const
{
    return accountFileStem(m_directory, account) + ".owned.json";
}
std::optional<QStringList> LocalCosmeticInventory::owned(const QString &account) const
{
    QFile file(path(account));
    if (!file.exists())
        return QStringList();
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject() || !document.object().value("owned").isArray())
        return std::nullopt;
    QStringList ids;
    for (const auto &value : document.object().value("owned").toArray()) {
        // Ids this build does not know are kept for the build that does.
        if (value.isString() && !value.toString().isEmpty() && !ids.contains(value.toString()))
            ids.append(value.toString());
    }
    return ids;
}
bool LocalCosmeticInventory::grant(const QString &account, const QStringList &ids)
{
    if (account.isEmpty() || !QDir().mkpath(m_directory))
        return false;
    QLockFile lock(path(account) + ".lock");
    if (!lock.tryLock(1000))
        return false;
    auto current = owned(account);
    if (!current)
        return false;
    QStringList updated = *current;
    for (const QString &id : ids) {
        if (!id.isEmpty() && !updated.contains(id))
            updated.append(id);
    }
    if (updated == *current)
        return true;
    const auto bytes = QJsonDocument(QJsonObject{{"owned", QJsonArray::fromStringList(updated)}}).toJson();
    QSaveFile output(path(account));
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}

LocalDailyCaseService::LocalDailyCaseService(QString directory)
    : m_directory(directory.isEmpty() ? defaultDailyCaseDirectory() : std::move(directory)),
      m_inventory(m_directory) {}
CaseReply LocalDailyCaseService::status(const QString &account) { return transact(account, false); }
CaseReply LocalDailyCaseService::claim(const QString &account) { return transact(account, true); }
CaseReply LocalDailyCaseService::transact(const QString &account, bool claim)
{
    if (account.isEmpty() || !QDir().mkpath(m_directory))
        return {{}, false, QStringLiteral("Could not access today's case. Please try again.")};
    const QString path = accountFileStem(m_directory, account) + ".json";
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
        return {{}, false, QStringLiteral("Your case is being updated in another window. Please try again.")};
    const auto collection = m_inventory.owned(account);
    if (!collection)
        return {{}, false, QStringLiteral("Could not read your collection.")};
    QStringList owned = *collection;
    // Hands a claim's reward over to the collection. A save that fails is
    // retried by the next transaction, which finds the same claim again.
    const auto keep = [&](const QString &id) {
        if (CosmeticCatalog::find(id) && !owned.contains(id) && m_inventory.grant(account, {id}))
            owned.append(id);
    };
    const auto now = QDateTime::currentDateTimeUtc(); // MOCK ONLY; server time in online adapter.
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly))
            return {{}, false, QStringLiteral("Could not read today's case.")};
        const auto obj = QJsonDocument::fromJson(file.readAll()).object();
        const auto next = QDateTime::fromString(obj.value("next").toString(), Qt::ISODate);
        if (!next.isValid() || obj.value("claim").toString().isEmpty() || !obj.value("seed").isDouble())
            return {{}, false, QStringLiteral("The saved case could not be read.")};
        // A claim saved before collections existed still hands its reward over.
        keep(obj.value("reward").toString());
        if (now < next)
            return {CaseResult{obj.value("claim").toString(),
                               obj.value("reward").toString(QStringLiteral("placeholder")),
                               quint32(obj.value("seed").toDouble()), next}, false, {}, owned};
    }
    if (!claim)
        return {{}, false, {}, owned};
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
    // Recorded as claimed first, so a failed grant is only ever retried,
    // never turned into a second draw.
    keep(result.rewardId);
    return {result, true, {}, owned};
}
}
