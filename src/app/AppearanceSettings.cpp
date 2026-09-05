#include "app/AppearanceSettings.h"

#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>

namespace OpenChat {

AppearanceSettings::AppearanceSettings(QObject *parent) : QObject(parent)
{
    m_darkMode = QSettings().value(QStringLiteral("Appearance/darkMode"), false).toBool();
    applyPalette();
}

void AppearanceSettings::setDarkMode(bool enabled)
{
    if (enabled == m_darkMode)
        return;
    m_darkMode = enabled;
    QSettings settings;
    settings.setValue(QStringLiteral("Appearance/darkMode"), enabled);
    settings.sync();
    applyPalette();
    emit darkModeChanged();
}

void AppearanceSettings::applyPalette()
{
    if (!qGuiApp)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Let native window decorations and platform dialogs follow the same choice.
    qGuiApp->styleHints()->setColorScheme(m_darkMode ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif
    QPalette palette;
    const QColor text(m_darkMode ? "#e0eaf3" : "#2b3b53");
    palette.setColor(QPalette::Window, QColor(m_darkMode ? "#18232e" : "#f8fbfd"));
    palette.setColor(QPalette::Base, QColor(m_darkMode ? "#111d28" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor(m_darkMode ? "#223341" : "#eef4f8"));
    palette.setColor(QPalette::Button, QColor(m_darkMode ? "#2b3e4e" : "#f5f8fa"));
    palette.setColor(QPalette::ToolTipBase, QColor(m_darkMode ? "#293e50" : "#f2faff"));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                            QPalette::ToolTipText}) {
        palette.setColor(role, text);
        palette.setColor(QPalette::Disabled, role, QColor(m_darkMode ? "#8092a3" : "#8b99aa"));
    }
    palette.setColor(QPalette::PlaceholderText, QColor(m_darkMode ? "#91a6b8" : "#98a7ba"));
    palette.setColor(QPalette::Highlight, QColor(m_darkMode ? "#32678d" : "#b9ddf5"));
    palette.setColor(QPalette::HighlightedText, QColor(m_darkMode ? "#ffffff" : "#20354a"));
    palette.setColor(QPalette::Link, QColor(m_darkMode ? "#91c8ed" : "#35618f"));
    QGuiApplication::setPalette(palette);
}

} // namespace OpenChat
