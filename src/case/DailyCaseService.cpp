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
#include <algorithm>

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

LocalDailyCaseService::LocalDailyCaseService(QString directory, qint64 dropIntervalMs)
    : m_directory(directory.isEmpty() ? defaultDailyCaseDirectory() : std::move(directory)),
      m_dropIntervalMs(std::max<qint64>(1, dropIntervalMs)),
      m_inventory(m_directory) {}
CaseReply LocalDailyCaseService::status(const QString &account) { return transact(account, Action::Status); }
CaseReply LocalDailyCaseService::claim(const QString &account) { return transact(account, Action::Claim); }
CaseReply LocalDailyCaseService::accrue(const QString &account, qint64 ms)
{
    return transact(account, Action::Accrue, ms);
}
CaseReply LocalDailyCaseService::transact(const QString &account, Action action, qint64 ms)
{
    if (account.isEmpty() || !QDir().mkpath(m_directory))
        return {{}, false, QStringLiteral("Could not access your case. Please try again.")};
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
    // The account's record. None yet is a new account's: one case waits, so
    // the first is there to open straight away.
    CaseResult last;
    int drops = 1;
    qint64 progress = 0;
    QFile file(path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly))
            return {{}, false, QStringLiteral("Could not read your case.")};
        const auto obj = QJsonDocument::fromJson(file.readAll()).object();
        last.claimId = obj.value("claim").toString();
        const bool seeded = obj.value("seed").isDouble();
        if (obj.contains("drops")) {
            drops = int(obj.value("drops").toDouble(-1));
            progress = qint64(obj.value("progressMs").toDouble(-1));
            if (drops < 0 || progress < 0 || (!last.claimId.isEmpty() && !seeded))
                return {{}, false, QStringLiteral("The saved case could not be read.")};
        } else {
            // Saved by a build with a cooldown: one case waits if it was due.
            const auto next = QDateTime::fromString(obj.value("next").toString(), Qt::ISODate);
            if (!next.isValid() || last.claimId.isEmpty() || !seeded)
                return {{}, false, QStringLiteral("The saved case could not be read.")};
            drops = QDateTime::currentDateTimeUtc() >= next ? 1 : 0;
            progress = 0;
        }
        last.rewardId = obj.value("reward").toString(QStringLiteral("placeholder"));
        last.seed = quint32(obj.value("seed").toDouble());
        last.caseKey = obj.value("case").toString(last.claimId);
        // A claim saved before collections existed still hands its reward over.
        keep(obj.value("reward").toString());
    }
    // The case on offer follows the last claim; before any, it is the first.
    const auto nextKey = [&] {
        return last.claimId.isEmpty() ? QStringLiteral("first") : QStringLiteral("after ") + last.claimId;
    };
    const auto reply = [&](bool newlyClaimed) {
        return CaseReply{last.claimId.isEmpty() ? std::nullopt : std::optional<CaseResult>(last),
                         newlyClaimed, {}, owned, nextKey(), drops, progress};
    };
    const auto save = [&] {
        QJsonObject obj{{"drops", drops}, {"progressMs", double(progress)}};
        if (!last.claimId.isEmpty()) {
            obj.insert("claim", last.claimId);
            obj.insert("reward", last.rewardId);
            obj.insert("seed", double(last.seed));
            obj.insert("case", last.caseKey);
        }
        const auto bytes = QJsonDocument(obj).toJson();
        QSaveFile output(path);
        return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
    };

    switch (action) {
    case Action::Status:
        return reply(false);
    case Action::Accrue: {
        // MOCK ONLY: the running time is the client's word for it.
        const qint64 credited = std::clamp<qint64>(ms, 0, std::max<qint64>(0, m_dropIntervalMs - progress));
        if (credited == 0 && progress < m_dropIntervalMs)
            return reply(false);
        progress += credited;
        if (progress >= m_dropIntervalMs) {
            ++drops;
            progress = 0;
        }
        if (!save())
            return {{}, false, QStringLiteral("Could not save your case progress.")};
        return reply(false);
    }
    case Action::Claim:
        break;
    }
    if (drops <= 0) {
        if (last.claimId.isEmpty())
            return {{}, false, QStringLiteral("There is no case to open yet.")};
        return reply(false);
    }
    // The reward is drawn here, by the authority, with the tier odds; the seed
    // below only arranges the reel. MOCK ONLY: the online adapter's server draws.
    auto *random = QRandomGenerator::global();
    const auto &reward = CosmeticCatalog::draw(random->bounded(quint32(CosmeticCatalog::totalWeight)),
                                               random->generate());
    last = CaseResult{QUuid::createUuid().toString(QUuid::WithoutBraces), reward.id,
                      random->generate(), nextKey()};
    --drops;
    if (!save())
        return {{}, false, QStringLiteral("Could not save your case. Please try again.")};
    // Recorded as claimed first, so a failed grant is only ever retried,
    // never turned into a second draw.
    keep(last.rewardId);
    return reply(true);
}
}
