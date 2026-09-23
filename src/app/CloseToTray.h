#pragma once

#include <QObject>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QWindow;
QT_END_NAMESPACE

namespace OpenChat {

// Closing the main window hides it into the notification area instead of
// ending the application, which from then on quits only when asked to: the
// tray's Close, a restart, the desktop session ending.
//
// Qt closes every window on its way out of the event loop, and a window that
// refuses cancels the quit. So a close only becomes a hide while no quit is
// under way, which the filter learns by seeing the application's quit request
// before those closes arrive. It watches the whole application for that, the
// one reason it is an application-wide filter.
//
// While it is installed, closing some other window (a crash notice, the voice
// debug window) no longer ends the application just because the main window
// is out of sight; removing it restores Qt's quit-on-last-window rule.
class CloseToTray final : public QObject
{
    Q_OBJECT

public:
    explicit CloseToTray(QWindow *window, QObject *parent = nullptr);
    ~CloseToTray() override;

    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPointer<QWindow> m_window;
    bool m_quitting = false;
    bool m_quitOnLastWindowClosed = true;
};

} // namespace OpenChat
