#pragma once

#include <QEvent>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

#include <functional>

class QQuickWindow;

namespace OpenChat {

// Delivered to a ProfileTicker subscriber (through QObject::customEvent) at
// the rate it subscribed with.
class ProfileTickEvent final : public QEvent
{
public:
    explicit ProfileTickEvent(int frame) : QEvent(eventType()), m_frame(frame) {}

    [[nodiscard]] static QEvent::Type eventType();
    // The ticker's own 30 fps frame number when this tick was sent.
    [[nodiscard]] int frame() const noexcept { return m_frame; }

private:
    int m_frame = 0;
};

// The one clock behind every continuous profile animation (SPEC §17): the
// ambient sprites at 30 fps, the name's glitter at 8, the song equaliser at
// 10, the loading spinner at 15. A single coarse 33 ms timer runs only while
// at least one subscriber is active and ProfileRenderPolicy allows animation;
// stopped means every subscriber keeps a stable frame.
//
// Native items subscribe while they are really on screen (visible, in a
// window that is neither hidden nor minimised) and unsubscribe otherwise;
// QML uses ProfileTickerClient with `active` bound to the same condition.
class ProfileTicker final : public QObject
{
    Q_OBJECT
    // Counts the clock's ticks while it runs; stands still while it is stopped.
    Q_PROPERTY(int frame READ frame NOTIFY frameChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

public:
    static constexpr int clockFps = 30;
    static constexpr int intervalMs = 33;

    // The process instance (never destroyed; GUI thread).
    static ProfileTicker &instance();

    [[nodiscard]] int frame() const noexcept { return m_frame; }
    [[nodiscard]] bool isRunning() const { return m_timer.isActive(); }

    // `client` receives a ProfileTickEvent `fps` times a second (1–30, a
    // share of the 30 fps clock). Subscribing again changes the rate. A
    // destroyed client is unsubscribed on its own.
    void subscribe(QObject *client, int fps);
    void unsubscribe(QObject *client);
    [[nodiscard]] bool isSubscribed(const QObject *client) const { return m_clients.contains(client); }
    [[nodiscard]] int subscriberCount() const { return int(m_clients.size()); }

signals:
    void frameChanged();
    void runningChanged();

private:
    ProfileTicker();

    struct Subscription final {
        int fps = clockFps;
        int credit = 0; // accumulates fps per tick; a tick is sent per clockFps of credit
        QMetaObject::Connection destroyed;
    };

    void updateRunning();
    void tick();

    QTimer m_timer;
    QHash<const QObject *, Subscription> m_clients;
    int m_frame = 0;
};

// QML's handle on the ticker: `ProfileTickerClient { active: …; fps: 10 }`
// counts its own frames while active and the clock runs.
class ProfileTickerClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
    Q_PROPERTY(int frame READ frame NOTIFY frameChanged)

public:
    explicit ProfileTickerClient(QObject *parent = nullptr);
    ~ProfileTickerClient() override;

    [[nodiscard]] bool active() const noexcept { return m_active; }
    void setActive(bool active);
    [[nodiscard]] int fps() const noexcept { return m_fps; }
    void setFps(int fps);
    [[nodiscard]] int frame() const noexcept { return m_frame; }

signals:
    void activeChanged();
    void fpsChanged();
    void frameChanged();

protected:
    void customEvent(QEvent *event) override;

private:
    void resubscribe();

    bool m_active = false;
    int m_fps = 10;
    int m_frame = 0;
};

// A native item's subscription, run only while the item is really on screen:
// it wants to animate, it and its parents are visible, it has a window, that
// window is neither hidden nor minimised (QWindow::visibilityChanged is
// watched, which the avatar frames' ticker does not do), and
// ProfileRenderPolicy allows animation. The owning item forwards its
// itemChange() here.
class ProfileItemAnimation final : public QObject
{
public:
    using TickFn = std::function<void(int frame)>;
    using StateFn = std::function<void(bool running)>;

    ProfileItemAnimation(QQuickItem *item, int fps, TickFn onTick, StateFn onRunningChanged = {});
    ~ProfileItemAnimation() override;

    void setWanted(bool wanted);
    [[nodiscard]] bool running() const noexcept { return m_running; }
    void itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &value);

protected:
    void customEvent(QEvent *event) override;

private:
    void watchWindow(QQuickWindow *window);
    void reevaluate();

    QQuickItem *m_item;
    int m_fps;
    TickFn m_onTick;
    StateFn m_onRunningChanged;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_windowVisibility;
    bool m_wanted = false;
    bool m_running = false;
};

} // namespace OpenChat
