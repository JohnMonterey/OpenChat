#include "case/RelayCaseService.h"

namespace OpenChat {

RelayCaseService::RelayCaseService(RelayClient *relay, QString localDirectory, QObject *parent)
    : QObject(parent)
    , m_relay(relay)
    , m_local(std::move(localDirectory))
{
    if (!relay) {
        m_unsupported = true;
        return;
    }
    connect(relay, &RelayClient::cosmeticsReceived, this, &RelayCaseService::onState);
    connect(relay, &RelayClient::caseClaimed, this, &RelayCaseService::onClaimed);
    connect(relay, &RelayClient::cosmeticsFailed, this, &RelayCaseService::onFailed);
    // The relay counts only while connected. Coming back also catches up on
    // anything that changed meanwhile (an operator's grant, say).
    connect(relay, &RelayClient::connected, this, [this] {
        if (!m_unsupported && m_relay)
            m_relay->fetchCosmetics();
    });
    connect(relay, &RelayClient::disconnected, this, [this] {
        if (!m_unsupported && m_state)
            push(replyFor(*m_state));
    });
}

RelayCaseService::~RelayCaseService() = default;

CaseReply RelayCaseService::failure(const QString &message)
{
    CaseReply reply;
    reply.error = message;
    return reply;
}

CaseReply RelayCaseService::replyFor(const RelayCosmeticState &state) const
{
    CaseReply reply;
    if (state.last) {
        reply.result = CaseResult{QString::fromLatin1(state.last->claimId.toHex()), state.last->rewardId,
                                  state.last->seed, state.last->caseKey};
    }
    reply.owned = state.owned;
    reply.caseKey = state.nextCaseKey;
    reply.drops = state.drops;
    reply.progressMs = state.progressMs;
    reply.loadout = state.loadout;
    reply.counting = m_relay && m_relay->isConnected();
    return reply;
}

qint64 RelayCaseService::dropIntervalMs() const
{
    if (m_unsupported)
        return m_local.dropIntervalMs();
    return m_state && m_state->intervalMs > 0 ? m_state->intervalMs : caseDropIntervalMs;
}

void RelayCaseService::status(const QString &account, Reply done)
{
    m_account = account;
    if (m_unsupported || !m_relay) {
        done(m_local.status(account));
        return;
    }
    m_waitingStatus.append(std::move(done));
    if (m_waitingStatus.size() == 1)
        m_relay->fetchCosmetics();
}

void RelayCaseService::claim(const QString &account, const QByteArray &requestId, Reply done)
{
    m_account = account;
    if (m_unsupported || !m_relay) {
        done(m_local.claim(account));
        return;
    }
    m_waitingClaims.append({requestId, std::move(done)});
    m_relay->claimCase(requestId);
}

void RelayCaseService::accrue(const QString &account, qint64 ms, Reply done)
{
    // Only the local stand-in is told the running time; the relay counts it.
    if (m_unsupported) {
        done(m_local.accrue(account, ms));
        return;
    }
    done(m_state ? replyFor(*m_state) : failure(QStringLiteral("Your cases are not loaded yet.")));
}

void RelayCaseService::equip(const QString &account, const QString &slot, const QString &itemId,
                             Reply done)
{
    m_account = account;
    if (m_unsupported || !m_relay) {
        // The loadout stays with AppearanceSettings on this device.
        done({});
        return;
    }
    m_waitingEquip.append(std::move(done));
    m_relay->equipCosmetic(slot, itemId);
}

void RelayCaseService::onState(const RelayCosmeticState &state)
{
    m_state = state;
    const CaseReply reply = replyFor(state);
    // Whoever is waiting wants the latest state; with nobody waiting it is news.
    auto statusWaiting = std::exchange(m_waitingStatus, {});
    auto equipWaiting = std::exchange(m_waitingEquip, {});
    if (statusWaiting.isEmpty() && equipWaiting.isEmpty())
        push(reply);
    for (const Reply &done : statusWaiting)
        done(reply);
    for (const Reply &done : equipWaiting)
        done(reply);
    if (!state.imported && !m_importing && !m_account.isEmpty())
        importLocalCollection();
}

void RelayCaseService::onClaimed(const RelayCosmeticState &state, const RelayCosmeticClaim &claim,
                                 bool newlyClaimed)
{
    m_state = state;
    CaseReply reply = replyFor(state);
    reply.result = CaseResult{QString::fromLatin1(claim.claimId.toHex()), claim.rewardId, claim.seed,
                              claim.caseKey};
    reply.newlyClaimed = newlyClaimed;
    if (m_waitingClaims.isEmpty()) {
        push(replyFor(state));
        return;
    }
    const Reply done = m_waitingClaims.takeFirst().second;
    done(reply);
}

void RelayCaseService::onFailed(RelayCosmeticsCall call, int httpStatus)
{
    // A relay without the cosmetics routes: carry on as this device did.
    if (httpStatus == 404 && (call == RelayCosmeticsCall::State || call == RelayCosmeticsCall::Claim)) {
        fallBackToLocal();
        return;
    }
    const QString unreachable = QStringLiteral("Could not reach your cases. Please try again.");
    switch (call) {
    case RelayCosmeticsCall::State:
        for (const Reply &done : std::exchange(m_waitingStatus, {}))
            done(failure(unreachable));
        return;
    case RelayCosmeticsCall::Claim: {
        if (m_waitingClaims.isEmpty())
            return;
        const Reply done = m_waitingClaims.takeFirst().second;
        // 409: nothing was waiting after all (another device opened it).
        done(failure(httpStatus == 409 ? QStringLiteral("There is no case to open yet.") : unreachable));
        if (m_relay)
            m_relay->fetchCosmetics();
        return;
    }
    case RelayCosmeticsCall::Equip:
        for (const Reply &done : std::exchange(m_waitingEquip, {}))
            done(failure(QStringLiteral("That could not be worn.")));
        // What the relay has is what is worn.
        if (m_relay)
            m_relay->fetchCosmetics();
        return;
    case RelayCosmeticsCall::Import:
        m_importing = false;
        return;
    case RelayCosmeticsCall::Loadouts:
        return;
    }
}

void RelayCaseService::fallBackToLocal()
{
    m_unsupported = true;
    for (const Reply &done : std::exchange(m_waitingStatus, {}))
        done(m_local.status(m_account));
    for (const auto &[requestId, done] : std::exchange(m_waitingClaims, {}))
        done(m_local.claim(m_account));
    for (const Reply &done : std::exchange(m_waitingEquip, {}))
        done({});
}

void RelayCaseService::importLocalCollection()
{
    if (!m_relay)
        return;
    m_importing = true;
    // What this device kept before the relay held it: the collection, and the
    // waiting cases (a device that never opened one still has its first).
    const QStringList owned =
        LocalCosmeticInventory(m_local.directory()).owned(m_account).value_or(QStringList{});
    const CaseReply local = m_local.status(m_account);
    m_relay->importCosmetics(owned, local.error.isEmpty() ? local.drops : 0);
}

} // namespace OpenChat
