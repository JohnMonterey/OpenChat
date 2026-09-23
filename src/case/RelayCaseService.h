#pragma once

#include "case/DailyCaseService.h"
#include "network/RelayClient.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <optional>
#include <utility>

namespace OpenChat {

// The relay as the authority for cases and cosmetics, once signed in: it
// counts the time the account is connected, draws every reward, holds the
// collection and what is worn, and pushes a new case the moment it drops.
//
// Two bridges for the move from this device to the relay:
//  - a relay that does not hold cosmetics yet (it answers 404) is left alone:
//    this service then answers from the local stand-in, as before;
//  - the first time the relay reports an account it has not imported, the
//    collection and waiting cases kept on this device are handed over once
//    (the relay caps and records the import).
class RelayCaseService final : public QObject, public DailyCaseService
{
    Q_OBJECT
public:
    explicit RelayCaseService(RelayClient *relay, QString localDirectory = {},
                              QObject *parent = nullptr);
    ~RelayCaseService() override;

    void status(const QString &account, Reply done) override;
    void claim(const QString &account, const QByteArray &requestId, Reply done) override;
    [[nodiscard]] bool needsRunningTime() const override { return m_unsupported; }
    void accrue(const QString &account, qint64 ms, Reply done) override;
    void equip(const QString &account, const QString &slot, const QString &itemId, Reply done) override;
    [[nodiscard]] qint64 dropIntervalMs() const override;

    // True once the relay turned out not to hold cosmetics.
    [[nodiscard]] bool usingLocalStandIn() const { return m_unsupported; }

private:
    void onState(const RelayCosmeticState &state);
    void onClaimed(const RelayCosmeticState &state, const RelayCosmeticClaim &claim, bool newlyClaimed);
    void onFailed(RelayCosmeticsCall call, int httpStatus);
    void importLocalCollection();
    [[nodiscard]] CaseReply replyFor(const RelayCosmeticState &state) const;
    [[nodiscard]] static CaseReply failure(const QString &message);
    // The relay does not hold cosmetics: answer everything waiting locally.
    void fallBackToLocal();

    QPointer<RelayClient> m_relay;
    LocalDailyCaseService m_local;
    QString m_account;
    std::optional<RelayCosmeticState> m_state;
    QList<Reply> m_waitingStatus;
    QList<Reply> m_waitingEquip;
    QList<std::pair<QByteArray, Reply>> m_waitingClaims;
    bool m_unsupported = false;
    bool m_importing = false;
};

} // namespace OpenChat
