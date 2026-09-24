#include "app/MemorySettings.h"

#include "profile/ProfileMediaStore.h"
#include "profile/ProfileRenderPolicy.h"
#include "render/AvatarStore.h"

#include <QCoreApplication>
#include <QSettings>

#include <chrono>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace OpenChat {

namespace {

constexpr auto keyLowMemoryMode = "Performance/lowMemoryMode";

MemorySettings *s_instance = nullptr;
bool s_restartRequested = false;

bool savedLowMemoryMode()
{
    return QSettings().value(QLatin1String(keyLowMemoryMode), false).toBool();
}

} // namespace

MemorySettings::MemorySettings(QObject *parent)
    : QObject(parent)
    , m_lowMemoryMode(savedLowMemoryMode())
{
    if (!s_instance)
        s_instance = this;
    m_trimTimer.setInterval(std::chrono::minutes(1));
    connect(&m_trimTimer, &QTimer::timeout, this, &MemorySettings::releaseFreedHeap);
    apply();
}

MemorySettings::~MemorySettings()
{
    if (s_instance == this)
        s_instance = nullptr;
}

MemorySettings *MemorySettings::instance()
{
    return s_instance;
}

bool MemorySettings::lowMemoryModeAtStartup()
{
    static const bool atStartup = savedLowMemoryMode();
    return atStartup;
}

bool MemorySettings::restartRequested()
{
    return s_restartRequested;
}

void MemorySettings::releaseFreedHeap()
{
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

void MemorySettings::setLowMemoryMode(bool enabled)
{
    if (enabled == m_lowMemoryMode)
        return;
    m_lowMemoryMode = enabled;
    QSettings settings;
    settings.setValue(QLatin1String(keyLowMemoryMode), enabled);
    settings.sync();
    apply();
    emit lowMemoryModeChanged();
}

bool MemorySettings::restartPending() const
{
    return m_lowMemoryMode != lowMemoryModeAtStartup();
}

void MemorySettings::restartApplication()
{
    s_restartRequested = true;
    QCoreApplication::quit();
}

void MemorySettings::apply()
{
    AvatarStore::instance().setKeepDecoded(!m_lowMemoryMode);
    // Profile pages follow at once too: their animations (name glitter,
    // falling effects, transitions) hold still, and a page's background
    // picture is decoded only while a page shows it, one at a time.
    ProfileRenderPolicy::instance().setLowMemoryMode(m_lowMemoryMode);
    ProfileMediaStore::instance().setKeepDecoded(!m_lowMemoryMode);
    if (m_lowMemoryMode) {
        releaseFreedHeap();
        m_trimTimer.start();
    } else {
        m_trimTimer.stop();
    }
}

} // namespace OpenChat
