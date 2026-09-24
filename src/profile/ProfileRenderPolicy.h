#pragma once

#include <QObject>

namespace OpenChat {

// What the profile renderer may spend on motion: a QML singleton every
// animated profile piece consults (the shared ticker, the name's glitter, the
// ambient sprites, the page's transitions). Low memory mode and the
// platform's reduced-motion preference both still every animation, leaving a
// stable frame.
class ProfileRenderPolicy final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool lowMemoryMode READ lowMemoryMode NOTIFY changed)
    Q_PROPERTY(bool reducedMotion READ reducedMotion NOTIFY changed)
    Q_PROPERTY(bool animationsAllowed READ animationsAllowed NOTIFY changed)

public:
    // The process instance. It lives until the process ends (never destroyed,
    // so nothing depends on static destruction order after the application
    // object is gone), and belongs to the thread that first asks for it: the
    // GUI thread.
    static ProfileRenderPolicy &instance();

    [[nodiscard]] bool lowMemoryMode() const noexcept { return m_lowMemoryMode; }
    [[nodiscard]] bool reducedMotion() const noexcept { return m_reducedMotion; }
    [[nodiscard]] bool animationsAllowed() const noexcept { return !m_lowMemoryMode && !m_reducedMotion; }

    // Pushed by MemorySettings::apply().
    void setLowMemoryMode(bool enabled);
    // The platform probe runs at construction; tests override it here.
    void setReducedMotion(bool reduced);
    // Back to a fresh process's state: Low memory off, the platform's motion
    // preference.
    void resetForTesting();

signals:
    void changed();

private:
    ProfileRenderPolicy();

    // Qt reports no reduced-motion setting, so this asks the one platform that
    // has a readable switch (Windows: "Animate controls and elements inside
    // windows"). Everywhere else it answers false.
    [[nodiscard]] static bool platformPrefersReducedMotion();

    bool m_lowMemoryMode = false;
    bool m_reducedMotion = false;
};

} // namespace OpenChat
