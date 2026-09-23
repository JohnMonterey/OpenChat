#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <functional>
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
// case on offer next; it seeds that case's belt, never its reward. `loadout` is
// what the account wears (slot -> id) when the authority holds it; a local one
// leaves that to AppearanceSettings. `counting` is false while the authority is
// not counting time (the relay, while disconnected), so no next drop is due.
// All of them mean nothing when `error` is set.
struct CaseReply {
    std::optional<CaseResult> result;
    bool newlyClaimed = false;
    QString error;
    QStringList owned;
    QString caseKey;
    int drops = 0;
    qint64 progressMs = 0;
    std::optional<QHash<QString, QString>> loadout;
    bool counting = true;
};
// Where cases and collections come from: the relay when signed in
// (RelayCaseService), a local stand-in otherwise. Every call answers through
// `done` -- at once for a local authority, when the relay replies for a remote
// one -- and an authority never calls `done` after it is destroyed.
class DailyCaseService {
public:
    using Reply = std::function<void(const CaseReply &)>;
    virtual ~DailyCaseService() = default;
    virtual void status(const QString &account, Reply done) = 0;
    // Opens a waiting case; with none waiting, replays the last one opened.
    // Retrying with the same `requestId` returns the same case rather than
    // opening another.
    virtual void claim(const QString &account, const QByteArray &requestId, Reply done) = 0;
    // Whether the authority must be told the running time; the relay counts
    // the time an account is connected itself.
    [[nodiscard]] virtual bool needsRunningTime() const { return true; }
    // Counts `ms` of running time toward the next drop. One report credits at
    // most what the next drop still needs, so it adds one drop at most.
    virtual void accrue(const QString &account, qint64 ms, Reply done) = 0;
    // Wears `itemId` in `slot` (empty clears it) where the authority keeps the
    // loadout; a local one leaves that to AppearanceSettings and answers empty.
    virtual void equip(const QString &account, const QString &slot, const QString &itemId, Reply done)
    {
        Q_UNUSED(account); Q_UNUSED(slot); Q_UNUSED(itemId);
        done({});
    }
    [[nodiscard]] virtual qint64 dropIntervalMs() const { return caseDropIntervalMs; }
    // Replies the authority sends unasked: a case dropped, or the connection
    // came or went.
    void setPushHandler(Reply handler) { m_push = std::move(handler); }
protected:
    void push(const CaseReply &reply) const
    {
        if (m_push)
            m_push(reply);
    }
private:
    Reply m_push;
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
// The local stand-in, used when not signed in (previews, tests) and when the
// relay does not hold cosmetics yet. It keeps each account's waiting drops and
// running time, draws the reward it shows and grants it into the account's
// LocalCosmeticInventory, all in files on this device that anyone can edit;
// the relay (RelayCaseService) is the authority. It answers at once, and its
// plain calls below answer directly. (The DailyCase names predate drops.)
class LocalDailyCaseService final : public DailyCaseService {
public:
    explicit LocalDailyCaseService(QString directory = {}, qint64 dropIntervalMs = caseDropIntervalMs);
    CaseReply status(const QString &account);
    CaseReply claim(const QString &account);
    CaseReply accrue(const QString &account, qint64 ms);
    void status(const QString &account, Reply done) override { done(status(account)); }
    void claim(const QString &account, const QByteArray &requestId, Reply done) override
    {
        Q_UNUSED(requestId);
        done(claim(account));
    }
    void accrue(const QString &account, qint64 ms, Reply done) override { done(accrue(account, ms)); }
    qint64 dropIntervalMs() const override { return m_dropIntervalMs; }
    [[nodiscard]] const QString &directory() const { return m_directory; }
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
