#include "app/CloseToTray.h"

#include <QEvent>
#include <QGuiApplication>
#include <QTimer>
#include <QWindow>

namespace OpenChat {

CloseToTray::CloseToTray(QWindow *window, QObject *parent)
    : QObject(parent),
      m_window(window),
      m_quitOnLastWindowClosed(QGuiApplication::quitOnLastWindowClosed())
{
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::instance()->installEventFilter(this);
}

CloseToTray::~CloseToTray()
{
    QGuiApplication::setQuitOnLastWindowClosed(m_quitOnLastWindowClosed);
}

bool CloseToTray::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Quit && watched == QCoreApplication::instance()) {
        m_quitting = true;
        // Another window may still refuse to close and cancel this quit, and
        // the window must go back to hiding if it does.
        QTimer::singleShot(0, this, [this] { m_quitting = false; });
    } else if (event->type() == QEvent::Close && watched == m_window && !m_quitting) {
        event->ignore();
        m_window->hide();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace OpenChat
