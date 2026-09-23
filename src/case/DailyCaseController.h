#pragma once
#include "CaseAudio.h"
#include "CaseMotion.h"
#include "DailyCaseService.h"
#include <QElapsedTimer>
#include <QObject>
#include <QVariantAnimation>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace OpenChat {
class DailyCaseController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accountKey READ accountKey WRITE setAccountKey NOTIFY changed)
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(int winnerIndex READ winnerIndex NOTIFY changed)
    Q_PROPERTY(int tileCount READ tileCount CONSTANT)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool muted MEMBER m_muted NOTIFY preferencesChanged)
    Q_PROPERTY(bool reducedMotion MEMBER m_reducedMotion NOTIFY preferencesChanged)
    // The opened item as a catalogue map ({id, category, name, rarity,
    // rarityName, rarityColor, ...}); empty while a case waits to be opened.
    Q_PROPERTY(QVariantMap reward READ reward NOTIFY changed)
    // How many cases wait to be opened. One drops for every half hour this
    // controller runs (that is, OpenChat is open); none while it is closed.
    Q_PROPERTY(int drops READ drops NOTIFY changed)
    // When the next case drops if OpenChat stays open; invalid until the
    // account's progress is known.
    Q_PROPERTY(QDateTime nextDropAt READ nextDropAt NOTIFY changed)
    // What every reel tile shows before the winner is known: the case's belt,
    // drawn with the same tier odds from the account and the service's case
    // key. It stays put when the claim lands, so nothing on screen swaps as the
    // spin starts, and the next case brings its own.
    Q_PROPERTY(QVariantList fillers READ fillers NOTIFY fillersChanged)
    // Everything this account has unboxed, as catalogue ids, and whether the
    // authority has said so yet: until it has (or after a failed reply for a
    // new account) the list is empty and means nothing.
    Q_PROPERTY(QStringList owned READ owned NOTIFY ownedChanged)
    Q_PROPERTY(bool ownershipKnown READ ownershipKnown NOTIFY ownedChanged)
    // What the account wears (slot -> catalogue id) where the authority keeps
    // it -- the relay, so it follows the account to every device -- and whether
    // it has said so. A local authority leaves this unknown.
    Q_PROPERTY(QVariantMap loadout READ loadout NOTIFY loadoutChanged)
    Q_PROPERTY(bool loadoutKnown READ loadoutKnown NOTIFY loadoutChanged)
public:
    // Available: a case waits, on show from the start of its belt. Opening: the
    // reel spins. Opened: the last case opened is on show, and stays until the
    // popup closes or the next case is opened; with nothing waiting, it stays.
    enum State { Available, Opening, Opened };
    Q_ENUM(State)
    // Without a service, a controller asks the factory the app installed (the
    // relay's, once signed in) or falls back to the local stand-in.
    explicit DailyCaseController(QObject *parent = nullptr);
    DailyCaseController(std::unique_ptr<DailyCaseService> service, QObject *parent = nullptr);
    ~DailyCaseController() override;
    using ServiceFactory = std::function<std::unique_ptr<DailyCaseService>()>;
    static void setServiceFactory(ServiceFactory factory);
    QString accountKey() const { return m_account; }
    void setAccountKey(const QString &account);
    int state() const { return m_state; }
    double position() const { return m_position; }
    int winnerIndex() const { return m_winner; }
    int tileCount() const { return CaseMotion::tileCount; }
    QString error() const { return m_error; }
    QVariantMap reward() const { return m_reward; }
    int drops() const { return m_drops; }
    QDateTime nextDropAt() const;
    QVariantList fillers() const { return m_fillers; }
    QStringList owned() const { return m_owned; }
    bool ownershipKnown() const { return m_ownershipKnown; }
    QVariantMap loadout() const { return m_loadout; }
    bool loadoutKnown() const { return m_loadoutKnown; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void open();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void display();
    // Wears `itemId` in `slot` (empty clears it) through the authority; the
    // answer arrives as the new loadout.
    Q_INVOKABLE void equip(const QString &slot, const QString &itemId);
signals:
    void changed();
    void positionChanged();
    void preferencesChanged();
    void fillersChanged();
    void ownedChanged();
    void loadoutChanged();
    void crossed(int index);
    void revealed();
private:
    void applyStatus(const CaseReply &reply);
    void applyClaim(const CaseReply &reply);
    void applyPush(const CaseReply &reply);
    void adopt(const CaseResult &result);
    void adoptOwned(const CaseReply &reply);
    void adoptDrops(const CaseReply &reply);
    void adoptLoadout(const CaseReply &reply);
    // Reports the running time not yet counted, then waits for the next drop.
    void reportRunningTime();
    qint64 untilNextDropMs() const;
    // The next waiting case, from the start of its own belt.
    void showNextCase();
    void arrangeBelt(const QString &caseKey);
    void setPosition(double position);
    void finish();
    std::unique_ptr<DailyCaseService> m_service;
    QString m_account, m_error;
    QVariantMap m_reward;
    QVariantList m_fillers;
    QStringList m_owned;
    bool m_ownershipKnown = false;
    QVariantMap m_loadout;
    bool m_loadoutKnown = false;
    int m_drops = 0;
    qint64 m_progressMs = 0;
    bool m_dropsKnown = false;
    bool m_counting = true;
    QString m_nextCaseKey;
    // A fresh reveal the popup has not been closed on yet.
    bool m_revealOnShow = false;
    // Replies for an account that is no longer this one are dropped.
    quint64 m_generation = 0;
    // The open in flight or last failed, retried as the same claim.
    QByteArray m_claimRequestId;
    bool m_claimPending = false;
    // The popup closed while the claim was on its way: land without a spin.
    bool m_quietClaim = false;
    // Running time since the last report, on a monotonic clock.
    QElapsedTimer m_running;
    // Time since the authority last said how far along the next drop is.
    QElapsedTimer m_sinceProgress;
    QTimer m_dropTimer;
    quint64 m_beltSeed = 0;
    State m_state = Available;
    int m_winner = CaseMotion::firstWinnerIndex;
    double m_position = CaseMotion::startIndex;
    bool m_muted = false, m_reducedMotion = false;
    QVariantAnimation m_animation;
    CaseAudio m_audio;
    QTimer m_audioRelease;
};
}
