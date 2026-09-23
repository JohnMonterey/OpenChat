#pragma once

#include <QObject>
#include <QString>

namespace OpenChat {

class AppearanceSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    // The equipped chat-bubble skin for the local user's own messages
    // ("bubble.aero", ...). Empty means the classic bubble. Local only.
    Q_PROPERTY(QString bubbleSkin READ bubbleSkin WRITE setBubbleSkin NOTIFY bubbleSkinChanged)
public:
    explicit AppearanceSettings(QObject *parent = nullptr);
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool enabled);
    QString bubbleSkin() const { return m_bubbleSkin; }
    void setBubbleSkin(const QString &skin);
signals:
    void darkModeChanged();
    void bubbleSkinChanged();
private:
    void applyPalette();
    bool m_darkMode = false;
    QString m_bubbleSkin;
};

} // namespace OpenChat
