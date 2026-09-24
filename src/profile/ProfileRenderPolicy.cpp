#include "profile/ProfileRenderPolicy.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace OpenChat {

ProfileRenderPolicy &ProfileRenderPolicy::instance()
{
    static auto *policy = new ProfileRenderPolicy;
    return *policy;
}

ProfileRenderPolicy::ProfileRenderPolicy() : m_reducedMotion(platformPrefersReducedMotion()) {}

bool ProfileRenderPolicy::platformPrefersReducedMotion()
{
#ifdef Q_OS_WIN
    // SPI_GETCLIENTAREAANIMATION is the switch Windows' "Show animations in
    // Windows" setting drives. A failed call leaves animations on.
    BOOL animate = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0))
        return !animate;
#endif
    return false;
}

void ProfileRenderPolicy::setLowMemoryMode(bool enabled)
{
    if (m_lowMemoryMode == enabled)
        return;
    m_lowMemoryMode = enabled;
    emit changed();
}

void ProfileRenderPolicy::setReducedMotion(bool reduced)
{
    if (m_reducedMotion == reduced)
        return;
    m_reducedMotion = reduced;
    emit changed();
}

void ProfileRenderPolicy::resetForTesting()
{
    const bool reduced = platformPrefersReducedMotion();
    if (!m_lowMemoryMode && m_reducedMotion == reduced)
        return;
    m_lowMemoryMode = false;
    m_reducedMotion = reduced;
    emit changed();
}

} // namespace OpenChat
