#include "app/AppearanceSettings.h"

#include "cosmetics/CosmeticCatalog.h"
#include "cosmetics/BubbleSkins.h"

#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>

namespace OpenChat {

namespace {

const QString frameKey = QStringLiteral("Appearance/avatarFrame");
const QString beadKey = QStringLiteral("Appearance/presenceBead");
const QString flairKey = QStringLiteral("Appearance/nameFlair");
const QString sceneKey = QStringLiteral("Appearance/profileScene");
const QString bubbleKey = QStringLiteral("Appearance/bubbleSkin");

// A stored id this build knows in `category`, or empty.
QString knownCosmetic(const QSettings &settings, const QString &key, const QString &category)
{
    const QString id = settings.value(key).toString();
    return CosmeticCatalog::isKnown(id, category) ? id : QString();
}

} // namespace

AppearanceSettings::AppearanceSettings(QObject *parent) : QObject(parent)
{
    const QSettings settings;
    m_darkMode = settings.value(QStringLiteral("Appearance/darkMode"), false).toBool();
    m_avatarFrame = knownCosmetic(settings, frameKey, QStringLiteral("frame"));
    m_presenceBead = knownCosmetic(settings, beadKey, QStringLiteral("bead"));
    m_nameFlair = knownCosmetic(settings, flairKey, QStringLiteral("flair"));
    m_profileScene = knownCosmetic(settings, sceneKey, QStringLiteral("scene"));
    m_bubbleSkin = knownCosmetic(settings, bubbleKey, QStringLiteral("bubble"));
    BubbleSkins::prepare(m_bubbleSkin);
    applyPalette();
}

bool AppearanceSettings::storeCosmetic(QString &field, const QString &id, const QString &category,
                                       const QString &key)
{
    const QString value = CosmeticCatalog::isKnown(id, category) ? id : QString();
    if (value == field)
        return false;
    field = value;
    QSettings settings;
    if (value.isEmpty())
        settings.remove(key);
    else
        settings.setValue(key, value);
    settings.sync();
    return true;
}

void AppearanceSettings::setAvatarFrame(const QString &id)
{
    if (storeCosmetic(m_avatarFrame, id, QStringLiteral("frame"), frameKey))
        emit avatarFrameChanged();
}

void AppearanceSettings::setPresenceBead(const QString &id)
{
    if (storeCosmetic(m_presenceBead, id, QStringLiteral("bead"), beadKey))
        emit presenceBeadChanged();
}

void AppearanceSettings::setNameFlair(const QString &id)
{
    if (storeCosmetic(m_nameFlair, id, QStringLiteral("flair"), flairKey))
        emit nameFlairChanged();
}

void AppearanceSettings::setProfileScene(const QString &id)
{
    if (storeCosmetic(m_profileScene, id, QStringLiteral("scene"), sceneKey))
        emit profileSceneChanged();
}

void AppearanceSettings::setBubbleSkin(const QString &skin)
{
    if (!storeCosmetic(m_bubbleSkin, skin, QStringLiteral("bubble"), bubbleKey))
        return;
    BubbleSkins::prepare(m_bubbleSkin);
    emit bubbleSkinChanged();
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
