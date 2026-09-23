#pragma once

#include <QObject>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QPlatformMenu;
class QPlatformMenuItem;
class QPlatformSystemTrayIcon;
QT_END_NAMESPACE

namespace OpenChat {

// OpenChat's icon in the notification area. Outside a call it is the
// application's own icon; in a voice call it is an orb saying what this
// user's microphone is doing (see TrayOrb). Clicking it asks for the window,
// and right-clicking offers Open and Close.
//
// Built directly on the platform's tray rather than on Qt.labs.platform or
// QSystemTrayIcon, because OpenChat is a QGuiApplication: Plasma's desktop
// theme builds its tray icons out of Qt Widgets, which abort a process that
// has no QApplication. So on the freedesktop platforms the icon is always
// Qt's own StatusNotifierItem, which needs nothing but D-Bus, while Windows
// and macOS get their native icon and menu.
class TrayIcon final : public QObject
{
    Q_OBJECT
    // "" outside a call, otherwise "call", "talking", "muted" or "deafened".
    Q_PROPERTY(QString callState READ callState WRITE setCallState NOTIFY callStateChanged)

public:
    // The icon, already showing, or null where this desktop has no
    // notification area: no StatusNotifierItem host, or a headless platform.
    static std::unique_ptr<TrayIcon> create(QObject *parent = nullptr);

    // Drives the given platform icon; `create` is how the application gets one.
    explicit TrayIcon(std::unique_ptr<QPlatformSystemTrayIcon> platformIcon,
                      QObject *parent = nullptr);
    ~TrayIcon() override;

    [[nodiscard]] QString callState() const { return m_callState; }
    void setCallState(const QString &state);

    // What hovering the icon says.
    [[nodiscard]] QString toolTip() const;

signals:
    void callStateChanged();
    // Open from the menu, or a click on the icon.
    void openRequested();
    // Close from the menu: quit the application.
    void closeRequested();

private:
    std::unique_ptr<QPlatformMenuItem> addMenuItem(const QString &text);
    void showCallState();

    // Declared in this order so the icon lets go of the menu, and the menu
    // of its items, before either is deleted.
    std::unique_ptr<QPlatformMenuItem> m_openItem;
    std::unique_ptr<QPlatformMenuItem> m_closeItem;
    std::unique_ptr<QPlatformMenu> m_menu;
    std::unique_ptr<QPlatformSystemTrayIcon> m_platformIcon;
    QString m_callState;
};

} // namespace OpenChat
