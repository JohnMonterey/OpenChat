#include "profile/ProfileTicker.h"

#include "profile/ProfileRenderPolicy.h"

#include <QCoreApplication>
#include <QPointer>
#include <QVector>

#include <algorithm>

namespace OpenChat {

QEvent::Type ProfileTickEvent::eventType()
{
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

ProfileTicker &ProfileTicker::instance()
{
    static auto *ticker = new ProfileTicker;
    return *ticker;
}

ProfileTicker::ProfileTicker()
{
    m_timer.setTimerType(Qt::CoarseTimer);
    m_timer.setInterval(intervalMs);
    connect(&m_timer, &QTimer::timeout, this, &ProfileTicker::tick);
    connect(&ProfileRenderPolicy::instance(), &ProfileRenderPolicy::changed, this,
            &ProfileTicker::updateRunning);
}

void ProfileTicker::subscribe(QObject *client, int fps)
{
    if (!client)
        return;
    fps = std::clamp(fps, 1, clockFps);
    auto existing = m_clients.find(client);
    if (existing != m_clients.end()) {
        existing->fps = fps;
        return;
    }
    Subscription subscription;
    subscription.fps = fps;
    // The pointer is only a key from here on: a client that dies without
    // unsubscribing is dropped before the next tick could reach it.
    subscription.destroyed = connect(client, &QObject::destroyed, this,
                                     [this, client] { unsubscribe(client); });
    m_clients.insert(client, subscription);
    updateRunning();
}

void ProfileTicker::unsubscribe(QObject *client)
{
    const auto it = m_clients.find(client);
    if (it == m_clients.end())
        return;
    disconnect(it->destroyed);
    m_clients.erase(it);
    updateRunning();
}

void ProfileTicker::updateRunning()
{
    const bool run = !m_clients.isEmpty() && ProfileRenderPolicy::instance().animationsAllowed();
    if (run == m_timer.isActive())
        return;
    if (run)
        m_timer.start();
    else
        m_timer.stop();
    emit runningChanged();
}

void ProfileTicker::tick()
{
    ++m_frame;
    // Decide who is due before sending anything: a client's tick handler may
    // subscribe, unsubscribe or delete other clients.
    QVector<QPointer<QObject>> due;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        it->credit += it->fps;
        if (it->credit >= clockFps) {
            it->credit -= clockFps;
            due.push_back(QPointer<QObject>(const_cast<QObject *>(it.key())));
        }
    }
    for (const QPointer<QObject> &client : due) {
        if (!client || !m_clients.contains(client.data()))
            continue;
        ProfileTickEvent event(m_frame);
        QCoreApplication::sendEvent(client.data(), &event);
    }
    emit frameChanged();
}

ProfileTickerClient::ProfileTickerClient(QObject *parent) : QObject(parent) {}

ProfileTickerClient::~ProfileTickerClient()
{
    ProfileTicker::instance().unsubscribe(this);
}

void ProfileTickerClient::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    resubscribe();
    emit activeChanged();
}

void ProfileTickerClient::setFps(int fps)
{
    fps = std::clamp(fps, 1, ProfileTicker::clockFps);
    if (m_fps == fps)
        return;
    m_fps = fps;
    resubscribe();
    emit fpsChanged();
}

void ProfileTickerClient::resubscribe()
{
    if (m_active)
        ProfileTicker::instance().subscribe(this, m_fps);
    else
        ProfileTicker::instance().unsubscribe(this);
}

void ProfileTickerClient::customEvent(QEvent *event)
{
    if (event->type() != ProfileTickEvent::eventType()) {
        QObject::customEvent(event);
        return;
    }
    ++m_frame;
    emit frameChanged();
}

} // namespace OpenChat
