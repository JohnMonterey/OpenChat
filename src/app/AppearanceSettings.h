#pragma once

#include <QObject>
#include <QString>

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
public:
    explicit AppearanceSettings(QObject *parent = nullptr);
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool enabled);

    QString avatarFrame() const { return m_avatarFrame; }
    void setAvatarFrame(const QString &id);
    QString presenceBead() const { return m_presenceBead; }
    void setPresenceBead(const QString &id);
    QString nameFlair() const { return m_nameFlair; }
    void setNameFlair(const QString &id);
    QString profileScene() const { return m_profileScene; }
    void setProfileScene(const QString &id);
signals:
    void darkModeChanged();
    void avatarFrameChanged();
    void presenceBeadChanged();
    void nameFlairChanged();
    void profileSceneChanged();
private:
    void applyPalette();
    // Validates, stores and persists one equipped cosmetic; true if it changed.
    bool storeCosmetic(QString &field, const QString &id, const QString &category,
                       const QString &key);
    bool m_darkMode = false;
    QString m_avatarFrame;
    QString m_presenceBead;
    QString m_nameFlair;
    QString m_profileScene;
};

} // namespace OpenChat
