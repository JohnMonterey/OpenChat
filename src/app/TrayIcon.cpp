#include "app/TrayIcon.h"

#include "render/TrayOrb.h"

#include <QDebug>
#include <QGuiApplication>
#include <QIcon>

#include <private/qguiapplication_p.h>
#include <qpa/qplatformmenu.h>
#include <qpa/qplatformsystemtrayicon.h>
#include <qpa/qplatformtheme.h>

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
#include <private/qgenericunixtheme_p.h>
#endif

namespace OpenChat {

std::unique_ptr<TrayIcon> TrayIcon::create(QObject *parent)
{
    const QString platform = QGuiApplication::platformName();
    std::unique_ptr<QPlatformSystemTrayIcon> platformIcon;
    if (platform == u"windows" || platform == u"cocoa") {
        if (QPlatformTheme *theme = QGuiApplicationPrivate::platformTheme())
            platformIcon.reset(theme->createPlatformSystemTrayIcon());
    }
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    else if (platform == u"xcb" || platform.startsWith(u"wayland")) {
        // Qt's own theme, never the desktop's (see the class comment). It
        // makes a StatusNotifierItem only when a host has registered the
        // watcher, which is what makes it show anywhere.
        platformIcon.reset(QGenericUnixTheme().createPlatformSystemTrayIcon());
    }
#endif
    if (!platformIcon || !platformIcon->isSystemTrayAvailable())
        return nullptr;
    return std::make_unique<TrayIcon>(std::move(platformIcon), parent);
}

TrayIcon::TrayIcon(std::unique_ptr<QPlatformSystemTrayIcon> platformIcon, QObject *parent)
    : QObject(parent), m_platformIcon(std::move(platformIcon))
{
    // The tray's own menu where it has one (the StatusNotifierItem's, served
    // over D-Bus); otherwise the platform's native popup menu.
    m_menu.reset(m_platformIcon->createMenu());
    if (!m_menu) {
        if (QPlatformTheme *theme = QGuiApplicationPrivate::platformTheme())
            m_menu.reset(theme->createPlatformMenu());
    }
    if (m_menu) {
        m_openItem = addMenuItem(QStringLiteral("Open"));
        m_closeItem = addMenuItem(QStringLiteral("Close"));
    } else {
        qWarning("OpenChat: this platform has no menus for the notification-area icon.");
    }
    if (m_openItem)
        connect(m_openItem.get(), &QPlatformMenuItem::activated, this, &TrayIcon::openRequested);
    if (m_closeItem)
        connect(m_closeItem.get(), &QPlatformMenuItem::activated, this, &TrayIcon::closeRequested);

    connect(m_platformIcon.get(), &QPlatformSystemTrayIcon::activated, this,
            [this](QPlatformSystemTrayIcon::ActivationReason reason) {
                if (reason == QPlatformSystemTrayIcon::Trigger
                    || reason == QPlatformSystemTrayIcon::DoubleClick)
                    emit openRequested();
            });

    m_platformIcon->init();
    if (m_menu)
        m_platformIcon->updateMenu(m_menu.get());
    showCallState();
}

TrayIcon::~TrayIcon()
{
    m_platformIcon->cleanup();
}

void TrayIcon::setCallState(const QString &state)
{
    if (state == m_callState)
        return;
    m_callState = state;
    showCallState();
    emit callStateChanged();
}

QString TrayIcon::toolTip() const
{
    if (m_callState.isEmpty())
        return QStringLiteral("OpenChat");
    if (m_callState == u"muted")
        return QStringLiteral("OpenChat — in a call, muted");
    if (m_callState == u"deafened")
        return QStringLiteral("OpenChat — in a call, deafened");
    // Talking is left out on purpose: the text would flicker with every
    // sentence, and the orb already shows it.
    return QStringLiteral("OpenChat — in a call");
}

std::unique_ptr<QPlatformMenuItem> TrayIcon::addMenuItem(const QString &text)
{
    std::unique_ptr<QPlatformMenuItem> item(m_menu->createMenuItem());
    if (!item)
        return nullptr;
    // Tags only need to tell the items apart.
    item->setTag(reinterpret_cast<quintptr>(item.get()));
    item->setText(text);
    item->setRole(QPlatformMenuItem::NoRole);
    item->setEnabled(true);
    item->setVisible(true);
    m_menu->insertMenuItem(item.get(), nullptr);
    return item;
}

void TrayIcon::showCallState()
{
    const QIcon orb = TrayOrb::icon(m_callState);
    m_platformIcon->updateIcon(orb.isNull() ? QGuiApplication::windowIcon() : orb);
    m_platformIcon->updateToolTip(toolTip());
}

} // namespace OpenChat
