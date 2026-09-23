#pragma once
#include "CaseAudio.h"
#include "CaseMotion.h"
#include "DailyCaseService.h"
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
    // The claimed item as a catalogue map ({id, category, name, rarity,
    // rarityName, rarityColor, ...}); empty while a case is available.
    Q_PROPERTY(QVariantMap reward READ reward NOTIFY changed)
    // When the next case can be opened, while the last one's cooldown runs;
    // invalid while a case is available.
    Q_PROPERTY(QDateTime nextAvailableAt READ nextAvailableAt NOTIFY changed)
    // What every reel tile shows before the winner is known: the case's belt,
    // drawn with the same tier odds from the account and the service's case
    // key. It stays put when the claim lands, so nothing on screen swaps as the
    // spin starts, and changes when the next case becomes available.
    Q_PROPERTY(QVariantList fillers READ fillers NOTIFY fillersChanged)
    // Everything this account has unboxed, as catalogue ids, and whether the
    // authority has said so yet: until it has (or after a failed reply for a
    // new account) the list is empty and means nothing.
    Q_PROPERTY(QStringList owned READ owned NOTIFY ownedChanged)
    Q_PROPERTY(bool ownershipKnown READ ownershipKnown NOTIFY ownedChanged)
public:
    enum State { Available, Opening, Opened };
    Q_ENUM(State)
    explicit DailyCaseController(QObject *parent = nullptr);
    DailyCaseController(std::unique_ptr<DailyCaseService> service, QObject *parent = nullptr);
    QString accountKey() const { return m_account; }
    void setAccountKey(const QString &account);
    int state() const { return m_state; }
    double position() const { return m_position; }
    int winnerIndex() const { return m_winner; }
    int tileCount() const { return CaseMotion::tileCount; }
    QString error() const { return m_error; }
    QVariantMap reward() const { return m_reward; }
    QDateTime nextAvailableAt() const { return m_nextAvailableAt; }
    QVariantList fillers() const { return m_fillers; }
    QStringList owned() const { return m_owned; }
    bool ownershipKnown() const { return m_ownershipKnown; }
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void open();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void display();
signals:
    void changed();
    void positionChanged();
    void preferencesChanged();
    void fillersChanged();
    void ownedChanged();
    void crossed(int index);
    void revealed();
private:
    void adopt(const CaseResult &result);
    void adoptOwned(const CaseReply &reply);
    void arrangeBelt(const QString &caseKey);
    void waitForNextCase();
    void setPosition(double position);
    void finish();
    std::unique_ptr<DailyCaseService> m_service;
    QString m_account, m_error;
    QVariantMap m_reward;
    QDateTime m_nextAvailableAt;
    QVariantList m_fillers;
    QStringList m_owned;
    bool m_ownershipKnown = false;
    quint64 m_beltSeed = 0;
    State m_state = Available;
    int m_winner = CaseMotion::firstWinnerIndex;
    double m_position = CaseMotion::startIndex;
    bool m_muted = false, m_reducedMotion = false;
    QVariantAnimation m_animation;
    CaseAudio m_audio;
    QTimer m_audioRelease;
    QTimer m_nextCase;
};
}
