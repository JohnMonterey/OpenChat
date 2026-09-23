#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <array>

namespace OpenChat {

class AppearanceSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    // The local user's equipped profile cosmetics, by catalogue id (see
    // cosmetics/CosmeticCatalog.h). Empty means none, which renders exactly the
    // stock look; an id this build does not know reads back as empty too.
    // Local only: nothing here is published to contacts.
    Q_PROPERTY(QString avatarFrame READ avatarFrame WRITE setAvatarFrame NOTIFY avatarFrameChanged)
    Q_PROPERTY(QString presenceBead READ presenceBead WRITE setPresenceBead NOTIFY presenceBeadChanged)
    Q_PROPERTY(QString nameFlair READ nameFlair WRITE setNameFlair NOTIFY nameFlairChanged)
    Q_PROPERTY(QString profileScene READ profileScene WRITE setProfileScene NOTIFY profileSceneChanged)
    // The equipped chat-bubble skin for the local user's own messages
    // ("bubble.aero", ...). Empty means the classic bubble. Local only.
    Q_PROPERTY(QString bubbleSkin READ bubbleSkin WRITE setBubbleSkin NOTIFY bubbleSkinChanged)
    // What the account may wear: the catalogue ids it has unboxed, as the daily
    // case's authority reports them (Main.qml hands them over). Until they are
    // known nothing is worn. Knowing them drops any equipped id the account
    // does not own, and equipping one it does not own is refused.
    Q_PROPERTY(QStringList ownedCosmetics READ ownedCosmetics WRITE setOwnedCosmetics NOTIFY
                   ownedCosmeticsChanged)
    // What the account wears (slot -> id) as the relay holds it, adopted as it
    // is -- Main.qml hands it over from the case controller -- so the loadout
    // follows the account to every device. Adopting it never asks the relay to
    // equip anything; equipping here does, through equipRequested().
    Q_PROPERTY(QVariantMap loadout READ loadout WRITE setLoadout NOTIFY loadoutChanged)
public:
    explicit AppearanceSettings(QObject *parent = nullptr);
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool enabled);

    QString avatarFrame() const { return worn(m_avatarFrame); }
    void setAvatarFrame(const QString &id);
    QString presenceBead() const { return worn(m_presenceBead); }
    void setPresenceBead(const QString &id);
    QString nameFlair() const { return worn(m_nameFlair); }
    void setNameFlair(const QString &id);
    QString profileScene() const { return worn(m_profileScene); }
    void setProfileScene(const QString &id);
    QString bubbleSkin() const { return worn(m_bubbleSkin); }
    void setBubbleSkin(const QString &skin);

    QStringList ownedCosmetics() const { return m_owned; }
    void setOwnedCosmetics(const QStringList &ids);
    QVariantMap loadout() const;
    void setLoadout(const QVariantMap &loadout);
    bool owns(const QString &id) const { return m_ownershipKnown && m_owned.contains(id); }
signals:
    void darkModeChanged();
    void avatarFrameChanged();
    void presenceBeadChanged();
    void nameFlairChanged();
    void profileSceneChanged();
    void bubbleSkinChanged();
    void ownedCosmeticsChanged();
    void loadoutChanged();
    // Something was equipped here (`itemId` empty: the slot was cleared), for
    // the authority to hold.
    void equipRequested(const QString &slot, const QString &itemId);
private:
    // One equipped cosmetic: its field, kind, settings key and change signal.
    struct EquipSlot
    {
        QString AppearanceSettings::*field;
        QString category;
        QString key;
        void (AppearanceSettings::*changed)();
    };
    static const std::array<EquipSlot, 5> &equipSlots();
    QString worn(const QString &id) const { return m_ownershipKnown ? id : QString(); }
    void applyPalette();
    // Validates, stores and persists one equipped cosmetic; true if it changed.
    bool storeCosmetic(QString &field, const QString &id, const QString &category,
                       const QString &key);
    bool m_darkMode = false;
    QString m_avatarFrame;
    QString m_presenceBead;
    QString m_nameFlair;
    QString m_profileScene;
    QString m_bubbleSkin;
    QStringList m_owned;
    bool m_ownershipKnown = false;
};

} // namespace OpenChat
