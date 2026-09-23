#include "DailyCaseController.h"
#include "cosmetics/CosmeticCatalog.h"
#include <QCryptographicHash>
#include <QSettings>
#include <random>

namespace OpenChat {
DailyCaseController::DailyCaseController(QObject *parent)
    : DailyCaseController(std::make_unique<LocalDailyCaseService>(), parent) {}
DailyCaseController::DailyCaseController(std::unique_ptr<DailyCaseService> service, QObject *parent)
    : QObject(parent), m_service(std::move(service))
{
    m_muted = QSettings().value("DailyCase/muted", false).toBool();
    m_reducedMotion = QSettings().value("DailyCase/reducedMotion", false).toBool();
    connect(this, &DailyCaseController::preferencesChanged, this, [this] {
        QSettings().setValue("DailyCase/muted", m_muted);
        QSettings().setValue("DailyCase/reducedMotion", m_reducedMotion);
        if (m_muted) m_audio.stop();
    });
    m_animation.setStartValue(0.0);
    m_animation.setEndValue(1.0);
    m_animation.setDuration(CaseMotion::durationMs);
    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        if (m_state == Opening)
            setPosition(CaseMotion::startIndex + (m_winner - CaseMotion::startIndex)
                        * CaseMotion::progress(value.toDouble()));
    });
    connect(&m_animation, &QVariantAnimation::finished, this, &DailyCaseController::finish);
    m_audioRelease.setSingleShot(true);
    connect(&m_audioRelease, &QTimer::timeout, &m_audio, &CaseAudio::stop);
    m_nextDay.setSingleShot(true);
    connect(&m_nextDay, &QTimer::timeout, this, [this] {
        if (m_state == Opening) m_nextDay.start(1000);
        else refresh();
    });
}
void DailyCaseController::setAccountKey(const QString &account)
{
    if (account == m_account) return;
    dismiss();
    m_account = account;
    // Another account's collection never carries over, even if its own
    // cannot be read.
    if (m_ownershipKnown || !m_owned.isEmpty()) {
        m_owned.clear();
        m_ownershipKnown = false;
        emit ownedChanged();
    }
    refresh();
}
void DailyCaseController::adopt(const CaseResult &result)
{
    m_winner = CaseMotion::firstWinnerIndex + int(result.seed % CaseMotion::winnerVariants);
    m_reward = cosmeticToVariant(CosmeticCatalog::find(result.rewardId));
}
void DailyCaseController::adoptOwned(const CaseReply &reply)
{
    // A failed reply says nothing about the collection; keep what is known.
    if (!reply.error.isEmpty() || (m_ownershipKnown && reply.owned == m_owned))
        return;
    m_owned = reply.owned;
    m_ownershipKnown = true;
    emit ownedChanged();
}
void DailyCaseController::arrangeBelt()
{
    // Visual only: the reward never comes from here (the service draws it).
    const auto day = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
    const auto digest = QCryptographicHash::hash((m_account + '|' + day).toUtf8(),
                                                 QCryptographicHash::Sha256);
    quint64 seed = 0;
    for (int i = 0; i < 8; ++i)
        seed = (seed << 8) | quint8(digest[i]);
    if (seed == m_beltSeed && !m_fillers.isEmpty()) return;
    m_beltSeed = seed;
    std::mt19937_64 random(seed);
    m_fillers.clear();
    for (int i = 0; i < CaseMotion::tileCount; ++i) {
        const auto tierRoll = quint32(random() >> 32), itemRoll = quint32(random() >> 32);
        m_fillers.append(cosmeticToVariant(&CosmeticCatalog::draw(tierRoll, itemRoll)));
    }
    emit fillersChanged();
}
void DailyCaseController::refresh()
{
    if (m_state == Opening) return;
    const auto reply = m_service->status(m_account);
    adoptOwned(reply);
    m_nextDay.stop();
    m_error = reply.error;
    m_state = reply.result ? OpenedToday : Available;
    if (!reply.result) m_reward.clear();
    arrangeBelt();
    if (reply.result) {
        adopt(*reply.result);
        m_nextDay.start(int(std::clamp(QDateTime::currentDateTimeUtc().msecsTo(reply.result->nextAvailableAt),
                                     qint64(1000), qint64(86400000))));
    }
    setPosition(reply.result ? m_winner : CaseMotion::startIndex);
    emit changed();
}
void DailyCaseController::open()
{
    if (m_state != Available) return;
    m_state = Opening; // Guard before entering the claim adapter.
    m_error.clear();
    emit changed();
    const auto reply = m_service->claim(m_account);
    // Granted with the claim: the collection holds the item before the reel moves.
    adoptOwned(reply);
    m_error = reply.error;
    if (!reply.result) {
        m_state = Available;
        emit changed();
        return;
    }
    adopt(*reply.result); // Claim durably recorded BEFORE any motion or sound.
    m_nextDay.start(int(std::clamp(QDateTime::currentDateTimeUtc().msecsTo(reply.result->nextAvailableAt),
                                 qint64(1000), qint64(86400000))));
    if (!reply.newlyClaimed) {
        m_state = OpenedToday;
        setPosition(m_winner);
        emit changed();
        return;
    }
    emit changed();
    if (m_reducedMotion) { finish(); return; }
    m_audioRelease.stop();
    if (!m_muted) m_audio.start();
    m_animation.start();
}
void DailyCaseController::setPosition(double position)
{
    const int previous = CaseMotion::selectedIndex(m_position);
    m_position = position;
    if (m_state == Opening && !m_reducedMotion) {
        const int current = CaseMotion::selectedIndex(position);
        if (current > previous) {
            // A stalled frame is coalesced into one current tick, never a burst.
            emit crossed(current);
            if (!m_muted) m_audio.tick();
        }
    }
    emit positionChanged();
}
void DailyCaseController::finish()
{
    if (m_state != Opening) return;
    m_state = OpenedToday;
    setPosition(m_winner); // Exact integer endpoint, independent of viewport size.
    emit changed();
    emit revealed();
    if (!m_muted && !m_reducedMotion) {
        const int openMs = m_audio.impact();
        if (openMs > 0) m_audioRelease.start(openMs + 120);
    }
}
void DailyCaseController::display()
{
    if (m_muted) return;
    m_audioRelease.stop();
    m_audio.start();
    const int displayMs = m_audio.display();
    if (displayMs > 0) m_audioRelease.start(displayMs + 120);
}
void DailyCaseController::dismiss()
{
    m_animation.stop();
    m_audioRelease.stop();
    m_audio.stop();
    if (m_state == Opening) {
        m_state = OpenedToday;
        setPosition(m_winner);
        emit changed();
    }
}
}
