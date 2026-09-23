#pragma once

#include "PostgresStore.h"
#include "RelayTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace OpenChat::Relay {

// One case opened: the drawn catalogue id, the seed that only arranges the
// client's reel, and the key the case was offered under (which seeds its belt).
struct CosmeticClaim final {
    QByteArray claimId;
    QString rewardId;
    quint32 seed = 0;
    QString caseKey;
};

// What an account has: waiting cases and connected time toward the next, the
// last case opened, the key of the case on offer next, what it owns (in the
// order it arrived) and what it wears (slot -> item).
struct CosmeticState final {
    int drops = 0;
    qint64 progressMs = 0;
    qint64 dropIntervalMs = 0;
    std::optional<CosmeticClaim> last;
    QString nextCaseKey;
    QStringList owned;
    QHash<QString, QString> loadout;
    bool imported = false;
};

struct CosmeticClaimOutcome final {
    CosmeticState state;
    CosmeticClaim claim;
    bool newlyClaimed = false;
};

// The authority for collectible cosmetics (see migration 007 and
// domain/CosmeticRules). Cases drop from the time an account is connected,
// not from anything a client reports; the relay draws every reward; a claim
// is keyed by the client's request id, so a retry after a lost response
// returns the same case rather than opening a second; and an account can wear
// only what it owns, in the slot it belongs to. Every change to one account
// runs in a transaction holding that account's case row.
class CosmeticsService final
{
public:
    struct Policy final {
        qint64 dropIntervalMs = 30LL * 60'000; // two cases an hour connected
        int welcomeDrops = 1;                   // a new account's first case
        int maxImportedDrops = 10;              // a device's waiting cases, imported once
        int maxLoadoutLookups = 256;
        int maxGrantDrops = 1000;
    };

    explicit CosmeticsService(PostgresStore &store);
    CosmeticsService(PostgresStore &store, Policy policy);

    [[nodiscard]] qint64 dropIntervalMs() const { return m_policy.dropIntervalMs; }

    // The account's state; its first call records the welcome case.
    [[nodiscard]] Result<CosmeticState, RelayError> state(const AccountId &account);

    // Counts `ms` of time the account was connected; every full interval drops
    // a case and the remainder carries toward the next.
    [[nodiscard]] Result<CosmeticState, RelayError> accrue(const AccountId &account, qint64 ms);

    // Opens a waiting case: draws the reward, consumes the case and grants the
    // item together. The same request id returns the same claim (not newly
    // claimed) without consuming anything; with no case waiting and a new
    // request id it is a Conflict.
    [[nodiscard]] Result<CosmeticClaimOutcome, RelayError>
    claim(const AccountId &account, QByteArrayView requestId);

    // Wears `itemId` in `slot`, or clears the slot when `itemId` is empty. An
    // unknown slot, or an item of another slot, is InvalidRequest; an item the
    // account does not own is NotFound.
    [[nodiscard]] Result<CosmeticState, RelayError>
    equip(const AccountId &account, const QString &slot, const QString &itemId);

    // Brings a collection kept on a device before the relay held it: the items
    // this build knows, and up to maxImportedDrops waiting cases (never fewer
    // than the account already has). Only the first import counts; later ones
    // return the state unchanged.
    [[nodiscard]] Result<CosmeticState, RelayError>
    importCollection(const AccountId &account, const QStringList &owned, int drops);

    // What the given accounts wear. Accounts wearing nothing, or unknown, are
    // absent. More than maxLoadoutLookups ids is InvalidRequest.
    [[nodiscard]] Result<QHash<AccountId, QHash<QString, QString>>, RelayError>
    loadouts(const QList<AccountId> &accounts);

    // Operator grant: adds `drops` waiting cases to one account, or to every
    // account when none is named. Returns how many accounts it reached.
    [[nodiscard]] Result<int, RelayError>
    grantDrops(int drops, const std::optional<AccountId> &account = std::nullopt);

private:
    [[nodiscard]] bool ensureRow(const AccountId &account, RelayError *error);
    [[nodiscard]] Result<CosmeticState, RelayError> load(const AccountId &account);

    PostgresStore &m_store;
    Policy m_policy;
};

} // namespace OpenChat::Relay
