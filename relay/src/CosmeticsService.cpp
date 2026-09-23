#include "CosmeticsService.h"

#include "RelayCrypto.h"
#include "domain/CosmeticRules.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace OpenChat::Relay {

namespace {

constexpr int maxImportedItems = 256;

// The case on offer after `lastClaim` (or the first, before any).
QString caseKeyAfter(const QByteArray &lastClaim)
{
    return lastClaim.isEmpty() ? QStringLiteral("first")
                               : QStringLiteral("after ") + QString::fromLatin1(lastClaim.toHex());
}

quint32 randomWord()
{
    return qFromLittleEndian<quint32>(randomBytes(4).constData());
}

// Rolls back unless committed; keeps every early return in a transaction honest.
class Transaction final
{
public:
    explicit Transaction(QSqlDatabase &db) : m_db(db), m_open(db.transaction()) {}
    ~Transaction()
    {
        if (m_open)
            m_db.rollback();
    }
    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] bool commit()
    {
        m_open = false;
        if (m_db.commit())
            return true;
        m_db.rollback();
        return false;
    }

private:
    QSqlDatabase &m_db;
    bool m_open;
};

} // namespace

CosmeticsService::CosmeticsService(PostgresStore &store)
    : CosmeticsService(store, Policy{})
{
}

CosmeticsService::CosmeticsService(PostgresStore &store, Policy policy)
    : m_store(store)
    , m_policy(policy)
{
    m_policy.dropIntervalMs = std::max<qint64>(1, m_policy.dropIntervalMs);
}

bool CosmeticsService::ensureRow(const AccountId &account, RelayError *error)
{
    QSqlQuery insert(m_store.database());
    insert.prepare(QStringLiteral(
        "INSERT INTO cosmetic_cases (account_id, drops, progress_ms, updated_at_ms) "
        "VALUES (?, ?, 0, ?) ON CONFLICT (account_id) DO NOTHING"));
    insert.addBindValue(account.bytes());
    insert.addBindValue(m_policy.welcomeDrops);
    insert.addBindValue(m_store.nowMs());
    if (insert.exec())
        return true;
    // No such account: the foreign key refuses the row.
    *error = insert.lastError().nativeErrorCode() == QLatin1String("23503") ? RelayError::NotFound
                                                                            : RelayError::Internal;
    return false;
}

Result<CosmeticState, RelayError> CosmeticsService::load(const AccountId &account)
{
    using R = Result<CosmeticState, RelayError>;
    QSqlDatabase &db = m_store.database();
    CosmeticState state;
    state.dropIntervalMs = m_policy.dropIntervalMs;

    QSqlQuery cases(db);
    cases.prepare(QStringLiteral(
        "SELECT c.drops, c.progress_ms, c.imported, c.last_claim_id, "
        "       l.reward_id, l.seed, l.case_key "
        "FROM cosmetic_cases c LEFT JOIN cosmetic_claims l ON l.claim_id = c.last_claim_id "
        "WHERE c.account_id = ?"));
    cases.addBindValue(account.bytes());
    if (!cases.exec())
        return R::failure(RelayError::Internal);
    if (!cases.next())
        return R::failure(RelayError::NotFound);
    state.drops = cases.value(0).toInt();
    state.progressMs = cases.value(1).toLongLong();
    state.imported = cases.value(2).toBool();
    const QByteArray lastClaim = cases.value(3).toByteArray();
    if (!lastClaim.isEmpty()) {
        state.last = CosmeticClaim{lastClaim, cases.value(4).toString(),
                                   static_cast<quint32>(cases.value(5).toLongLong()),
                                   cases.value(6).toString()};
    }
    state.nextCaseKey = caseKeyAfter(lastClaim);

    QSqlQuery items(db);
    items.prepare(QStringLiteral(
        "SELECT item_id FROM cosmetic_items WHERE account_id = ? ORDER BY acquired_at_ms, item_id"));
    items.addBindValue(account.bytes());
    if (!items.exec())
        return R::failure(RelayError::Internal);
    while (items.next())
        state.owned.append(items.value(0).toString());

    QSqlQuery worn(db);
    worn.prepare(QStringLiteral("SELECT slot, item_id FROM cosmetic_loadouts WHERE account_id = ?"));
    worn.addBindValue(account.bytes());
    if (!worn.exec())
        return R::failure(RelayError::Internal);
    while (worn.next())
        state.loadout.insert(worn.value(0).toString(), worn.value(1).toString());
    return R::success(state);
}

Result<CosmeticState, RelayError> CosmeticsService::state(const AccountId &account)
{
    RelayError error = RelayError::Internal;
    if (!ensureRow(account, &error))
        return Result<CosmeticState, RelayError>::failure(error);
    return load(account);
}

Result<CosmeticState, RelayError> CosmeticsService::accrue(const AccountId &account, qint64 ms)
{
    using R = Result<CosmeticState, RelayError>;
    if (ms <= 0)
        return state(account);
    QSqlDatabase &db = m_store.database();
    Transaction tx(db);
    if (!tx.isOpen())
        return R::failure(RelayError::Internal);
    RelayError error = RelayError::Internal;
    if (!ensureRow(account, &error))
        return R::failure(error);
    QSqlQuery row(db);
    row.prepare(QStringLiteral(
        "SELECT drops, progress_ms FROM cosmetic_cases WHERE account_id = ? FOR UPDATE"));
    row.addBindValue(account.bytes());
    if (!row.exec() || !row.next())
        return R::failure(RelayError::Internal);
    const qint64 total = row.value(1).toLongLong() + ms;
    const qint64 dropped = total / m_policy.dropIntervalMs;
    const int drops = static_cast<int>(std::min<qint64>(row.value(0).toLongLong() + dropped,
                                                        std::numeric_limits<int>::max()));
    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE cosmetic_cases SET drops = ?, progress_ms = ?, updated_at_ms = ? "
        "WHERE account_id = ?"));
    update.addBindValue(drops);
    update.addBindValue(total % m_policy.dropIntervalMs);
    update.addBindValue(m_store.nowMs());
    update.addBindValue(account.bytes());
    if (!update.exec() || !tx.commit())
        return R::failure(RelayError::Internal);
    return load(account);
}

Result<CosmeticClaimOutcome, RelayError> CosmeticsService::claim(const AccountId &account,
                                                                 QByteArrayView requestId)
{
    using R = Result<CosmeticClaimOutcome, RelayError>;
    if (requestId.size() < 8 || requestId.size() > 32)
        return R::failure(RelayError::InvalidRequest);
    QSqlDatabase &db = m_store.database();
    Transaction tx(db);
    if (!tx.isOpen())
        return R::failure(RelayError::Internal);
    RelayError error = RelayError::Internal;
    if (!ensureRow(account, &error))
        return R::failure(error);

    // Holding the account's case row serializes its claims.
    QSqlQuery row(db);
    row.prepare(QStringLiteral(
        "SELECT drops, last_claim_id FROM cosmetic_cases WHERE account_id = ? FOR UPDATE"));
    row.addBindValue(account.bytes());
    if (!row.exec() || !row.next())
        return R::failure(RelayError::Internal);
    const int drops = row.value(0).toInt();
    const QByteArray lastClaim = row.value(1).toByteArray();

    // A retry of a claim already made returns that claim.
    QSqlQuery earlier(db);
    earlier.prepare(QStringLiteral(
        "SELECT claim_id, reward_id, seed, case_key FROM cosmetic_claims "
        "WHERE account_id = ? AND request_id = ?"));
    earlier.addBindValue(account.bytes());
    earlier.addBindValue(requestId.toByteArray());
    if (!earlier.exec())
        return R::failure(RelayError::Internal);
    if (earlier.next()) {
        const CosmeticClaim claim{earlier.value(0).toByteArray(), earlier.value(1).toString(),
                                  static_cast<quint32>(earlier.value(2).toLongLong()),
                                  earlier.value(3).toString()};
        if (!tx.commit())
            return R::failure(RelayError::Internal);
        auto state = load(account);
        if (!state.hasValue())
            return R::failure(state.error());
        return R::success(CosmeticClaimOutcome{state.value(), claim, false});
    }
    if (drops <= 0)
        return R::failure(RelayError::Conflict);

    // The relay draws; the seed only arranges the client's reel.
    const quint32 tierRoll = randomWord();
    const quint32 itemRoll = randomWord();
    const CosmeticRules::Item &reward = CosmeticRules::draw(tierRoll, itemRoll);
    const CosmeticClaim claim{randomBytes(16), QString::fromLatin1(reward.id), randomWord(),
                              caseKeyAfter(lastClaim)};
    const qint64 now = m_store.nowMs();

    QSqlQuery insert(db);
    insert.prepare(QStringLiteral(
        "INSERT INTO cosmetic_claims (claim_id, account_id, request_id, reward_id, seed, "
        "case_key, claimed_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(claim.claimId);
    insert.addBindValue(account.bytes());
    insert.addBindValue(requestId.toByteArray());
    insert.addBindValue(claim.rewardId);
    insert.addBindValue(static_cast<qint64>(claim.seed));
    insert.addBindValue(claim.caseKey);
    insert.addBindValue(now);
    if (!insert.exec())
        return R::failure(RelayError::Internal);

    QSqlQuery consume(db);
    consume.prepare(QStringLiteral(
        "UPDATE cosmetic_cases SET drops = drops - 1, last_claim_id = ?, updated_at_ms = ? "
        "WHERE account_id = ?"));
    consume.addBindValue(claim.claimId);
    consume.addBindValue(now);
    consume.addBindValue(account.bytes());
    if (!consume.exec())
        return R::failure(RelayError::Internal);

    QSqlQuery grant(db);
    grant.prepare(QStringLiteral(
        "INSERT INTO cosmetic_items (account_id, item_id, source, acquired_at_ms) "
        "VALUES (?, ?, 'case', ?) ON CONFLICT (account_id, item_id) DO NOTHING"));
    grant.addBindValue(account.bytes());
    grant.addBindValue(claim.rewardId);
    grant.addBindValue(now);
    if (!grant.exec() || !tx.commit())
        return R::failure(RelayError::Internal);

    auto state = load(account);
    if (!state.hasValue())
        return R::failure(state.error());
    return R::success(CosmeticClaimOutcome{state.value(), claim, true});
}

Result<CosmeticState, RelayError> CosmeticsService::equip(const AccountId &account,
                                                          const QString &slot,
                                                          const QString &itemId)
{
    using R = Result<CosmeticState, RelayError>;
    if (!CosmeticRules::slotNames().contains(slot))
        return R::failure(RelayError::InvalidRequest);
    if (!itemId.isEmpty() && !CosmeticRules::fits(itemId, slot))
        return R::failure(RelayError::InvalidRequest);
    RelayError error = RelayError::Internal;
    if (!ensureRow(account, &error))
        return R::failure(error);
    QSqlDatabase &db = m_store.database();

    if (itemId.isEmpty()) {
        QSqlQuery clear(db);
        clear.prepare(QStringLiteral("DELETE FROM cosmetic_loadouts WHERE account_id = ? AND slot = ?"));
        clear.addBindValue(account.bytes());
        clear.addBindValue(slot);
        if (!clear.exec())
            return R::failure(RelayError::Internal);
        return load(account);
    }

    QSqlQuery wear(db);
    wear.prepare(QStringLiteral(
        "INSERT INTO cosmetic_loadouts (account_id, slot, item_id) VALUES (?, ?, ?) "
        "ON CONFLICT (account_id, slot) DO UPDATE SET item_id = EXCLUDED.item_id"));
    wear.addBindValue(account.bytes());
    wear.addBindValue(slot);
    wear.addBindValue(itemId);
    if (!wear.exec()) {
        // Not in the account's collection: the foreign key refuses it.
        const bool unowned = wear.lastError().nativeErrorCode() == QLatin1String("23503");
        return R::failure(unowned ? RelayError::NotFound : RelayError::Internal);
    }
    return load(account);
}

Result<CosmeticState, RelayError> CosmeticsService::importCollection(const AccountId &account,
                                                                     const QStringList &owned,
                                                                     int drops)
{
    using R = Result<CosmeticState, RelayError>;
    if (owned.size() > maxImportedItems || drops < 0)
        return R::failure(RelayError::InvalidRequest);
    QSqlDatabase &db = m_store.database();
    Transaction tx(db);
    if (!tx.isOpen())
        return R::failure(RelayError::Internal);
    RelayError error = RelayError::Internal;
    if (!ensureRow(account, &error))
        return R::failure(error);
    QSqlQuery row(db);
    row.prepare(QStringLiteral(
        "SELECT drops, imported FROM cosmetic_cases WHERE account_id = ? FOR UPDATE"));
    row.addBindValue(account.bytes());
    if (!row.exec() || !row.next())
        return R::failure(RelayError::Internal);
    if (row.value(1).toBool()) {
        if (!tx.commit())
            return R::failure(RelayError::Internal);
        return load(account);
    }
    const qint64 now = m_store.nowMs();
    QSet<QString> seen;
    for (const QString &id : owned) {
        if (seen.contains(id) || !CosmeticRules::find(id))
            continue;
        seen.insert(id);
        QSqlQuery item(db);
        item.prepare(QStringLiteral(
            "INSERT INTO cosmetic_items (account_id, item_id, source, acquired_at_ms) "
            "VALUES (?, ?, 'import', ?) ON CONFLICT (account_id, item_id) DO NOTHING"));
        item.addBindValue(account.bytes());
        item.addBindValue(id);
        item.addBindValue(now);
        if (!item.exec())
            return R::failure(RelayError::Internal);
    }
    QSqlQuery update(db);
    update.prepare(QStringLiteral(
        "UPDATE cosmetic_cases SET drops = ?, imported = TRUE, updated_at_ms = ? WHERE account_id = ?"));
    update.addBindValue(std::max(row.value(0).toInt(), std::min(drops, m_policy.maxImportedDrops)));
    update.addBindValue(now);
    update.addBindValue(account.bytes());
    if (!update.exec() || !tx.commit())
        return R::failure(RelayError::Internal);
    return load(account);
}

Result<QHash<AccountId, QHash<QString, QString>>, RelayError>
CosmeticsService::loadouts(const QList<AccountId> &accounts)
{
    using Loadouts = QHash<AccountId, QHash<QString, QString>>;
    using R = Result<Loadouts, RelayError>;
    if (accounts.size() > m_policy.maxLoadoutLookups)
        return R::failure(RelayError::InvalidRequest);
    Loadouts result;
    if (accounts.isEmpty())
        return R::success(result);
    QStringList marks;
    marks.reserve(accounts.size());
    for (qsizetype i = 0; i < accounts.size(); ++i)
        marks.append(QStringLiteral("?"));
    QSqlQuery worn(m_store.database());
    worn.prepare(QStringLiteral("SELECT account_id, slot, item_id FROM cosmetic_loadouts "
                                "WHERE account_id IN (%1)").arg(marks.join(QLatin1Char(','))));
    for (const AccountId &account : accounts)
        worn.addBindValue(account.bytes());
    if (!worn.exec())
        return R::failure(RelayError::Internal);
    while (worn.next()) {
        const auto account = AccountId::fromBytes(worn.value(0).toByteArray());
        if (account)
            result[*account].insert(worn.value(1).toString(), worn.value(2).toString());
    }
    return R::success(result);
}

Result<int, RelayError> CosmeticsService::grantDrops(int drops, const std::optional<AccountId> &account)
{
    using R = Result<int, RelayError>;
    if (drops < 1 || drops > m_policy.maxGrantDrops)
        return R::failure(RelayError::InvalidRequest);
    QSqlQuery grant(m_store.database());
    grant.prepare(QStringLiteral(
        "INSERT INTO cosmetic_cases (account_id, drops, progress_ms, updated_at_ms) "
        "SELECT account_id, ?, 0, ? FROM accounts %1 "
        "ON CONFLICT (account_id) DO UPDATE "
        "SET drops = cosmetic_cases.drops + ?, updated_at_ms = EXCLUDED.updated_at_ms")
                      .arg(account ? QStringLiteral("WHERE account_id = ?") : QString()));
    grant.addBindValue(m_policy.welcomeDrops + drops);
    grant.addBindValue(m_store.nowMs());
    if (account)
        grant.addBindValue(account->bytes());
    grant.addBindValue(drops);
    if (!grant.exec())
        return R::failure(RelayError::Internal);
    const int reached = grant.numRowsAffected();
    if (account && reached == 0)
        return R::failure(RelayError::NotFound);
    return R::success(reached);
}

} // namespace OpenChat::Relay
