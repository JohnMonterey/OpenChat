#include "DailyCaseController.h"
#include "cosmetics/CosmeticCatalog.h"
#include <QCoreApplication>
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
    // One shot at the moment the next case is due; nothing polls meanwhile.
    m_dropTimer.setSingleShot(true);
    connect(&m_dropTimer, &QTimer::timeout, this, [this] {
        reportRunningTime();
        // A report that failed is tried again in a minute, time and all.
        if (!m_dropTimer.isActive() && m_running.isValid())
            m_dropTimer.start(60 * 1000);
        if (m_state == Opened && !m_revealOnShow && m_drops > 0)
            showNextCase();
        emit changed();
    });
    // Running time counts up to the moment OpenChat quits, and no further.
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &DailyCaseController::reportRunningTime);
}
DailyCaseController::~DailyCaseController()
{
    reportRunningTime();
}
void DailyCaseController::setAccountKey(const QString &account)
{
    if (account == m_account) return;
    dismiss();
    // The account that was open keeps the time it ran.
    reportRunningTime();
    m_account = account;
    // Another account's collection and cases never carry over, even if its
    // own cannot be read.
    if (m_ownershipKnown || !m_owned.isEmpty()) {
        m_owned.clear();
        m_ownershipKnown = false;
        emit ownedChanged();
    }
    m_drops = 0;
    m_progressMs = 0;
    m_dropsKnown = false;
    m_dropTimer.stop();
    if (m_account.isEmpty())
        m_running.invalidate();
    else
        m_running.start();
    m_state = Available;
    m_reward.clear();
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
void DailyCaseController::adoptDrops(const CaseReply &reply)
{
    // A failed reply says nothing about the cases either.
    if (!reply.error.isEmpty())
        return;
    m_drops = reply.drops;
    m_progressMs = reply.progressMs;
    m_nextCaseKey = reply.caseKey;
    m_dropsKnown = true;
    if (m_running.isValid())
        m_dropTimer.start(int(std::clamp<qint64>(untilNextDropMs(), 0, m_service->dropIntervalMs())));
}
qint64 DailyCaseController::untilNextDropMs() const
{
    const qint64 unreported = m_running.isValid() ? m_running.elapsed() : 0;
    return std::max<qint64>(0, m_service->dropIntervalMs() - m_progressMs - unreported);
}
QDateTime DailyCaseController::nextDropAt() const
{
    if (!m_dropsKnown || !m_running.isValid())
        return {};
    return QDateTime::currentDateTimeUtc().addMSecs(untilNextDropMs());
}
void DailyCaseController::reportRunningTime()
{
    if (m_account.isEmpty() || !m_running.isValid())
        return;
    const qint64 ms = m_running.elapsed();
    if (ms <= 0)
        return;
    const auto reply = m_service->accrue(m_account, ms);
    // Unreported, the time is carried into the next report instead.
    if (!reply.error.isEmpty())
        return;
    // A report counts no more than the next drop still needed, so time the
    // machine spent asleep past it is not kept either.
    m_running.restart();
    adoptOwned(reply);
    adoptDrops(reply);
}
void DailyCaseController::showNextCase()
{
    m_state = Available;
    m_reward.clear();
    m_revealOnShow = false;
    arrangeBelt(m_nextCaseKey);
    setPosition(CaseMotion::startIndex);
}
void DailyCaseController::arrangeBelt(const QString &caseKey)
{
    // Visual only: the reward never comes from here (the service draws it).
    const auto digest = QCryptographicHash::hash((m_account + '|' + caseKey).toUtf8(),
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
    adoptDrops(reply);
    m_error = reply.error;
    if (!reply.error.isEmpty()) {
        if (m_fillers.isEmpty())
            arrangeBelt({});
        emit changed();
        return;
    }
    if (m_state == Opened && m_revealOnShow) {
        // A fresh reveal stays on show until the popup closes.
    } else if (m_drops > 0) {
        showNextCase();
    } else if (reply.result) {
        m_state = Opened;
        adopt(*reply.result);
        arrangeBelt(reply.result->caseKey);
        setPosition(m_winner);
    } else {
        m_state = Opened;
        m_reward.clear();
        arrangeBelt(m_nextCaseKey);
        setPosition(CaseMotion::startIndex);
    }
    emit changed();
}
void DailyCaseController::open()
{
    if (m_state == Opening || m_drops <= 0) return;
    // After a reveal, the next case starts from its own belt.
    if (m_state == Opened)
        showNextCase();
    m_state = Opening; // Guard before entering the claim adapter.
    m_revealOnShow = false;
    m_error.clear();
    emit changed();
    const auto reply = m_service->claim(m_account);
    // Granted with the claim: the collection holds the item before the reel moves.
    adoptOwned(reply);
    adoptDrops(reply);
    m_error = reply.error;
    if (!reply.result) {
        m_state = Available;
        emit changed();
        return;
    }
    adopt(*reply.result); // Claim durably recorded BEFORE any motion or sound.
    if (!reply.newlyClaimed) {
        // Another window opened the last one; its result is what there is.
        m_state = Opened;
        arrangeBelt(reply.result->caseKey);
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
    m_state = Opened;
    m_revealOnShow = true;
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
    const State before = m_state;
    if (m_state == Opening) {
        m_state = Opened;
        setPosition(m_winner);
    }
    // Closing the popup puts the reveal away; the next waiting case is what
    // it shows when it opens again.
    m_revealOnShow = false;
    if (m_state == Opened && m_drops > 0)
        showNextCase();
    if (m_state != before)
        emit changed();
}
}
