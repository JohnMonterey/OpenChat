#pragma once

#include <QObject>

namespace OpenChat {

class AppearanceSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
public:
    explicit AppearanceSettings(QObject *parent = nullptr);
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool enabled);
signals:
    void darkModeChanged();
private:
    void applyPalette();
    bool m_darkMode = false;
};

} // namespace OpenChat
