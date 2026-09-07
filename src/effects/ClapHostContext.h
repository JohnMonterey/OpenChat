#pragma once

#include <clap/clap.h>

#include <QString>

#include <thread>

namespace OpenChat {

// The clap_host_t we present to every plugin, and the handful of host
// extensions behind it.
//
// A plugin is entitled to call get_extension for anything and to dereference
// what it gets back without checking, so the cost of not implementing an
// extension is not a missing feature -- it is a null dereference inside
// somebody else's code. The stubs here are therefore about being a well-formed
// host rather than about capability:
//
//   clap.log          The only way a plugin can say anything at all in a
//                     process with no console attached to it. Without it a
//                     misbehaving plugin fails silently.
//   clap.thread-check Answered HONESTLY against the real thread, never
//                     hardcoded to true. Plugins assert on this, and a
//                     flattering answer hides our threading bugs rather than
//                     theirs.
//   clap.latency      Plugins call changed() when their delay moves; we re-read
//                     it between frames rather than trusting the activation
//                     value forever.
//   clap.state        mark_dirty() is how a plugin says "there is something to
//                     save now".
//   clap.params       rescan/clear/request_flush, so a plugin that rearranges
//                     its own parameters is not left shouting into a null.
//
// Everything else returns null, which is legal and which real plugins tolerate:
// a host offering nothing at all still loads and runs the plugins on a typical
// Linux box. The stubs exist for the ones that do not, and because a host that
// answers thread-check dishonestly is lying to a debugger it will later need.
class ClapHostContext final
{
public:
    ClapHostContext();

    ClapHostContext(const ClapHostContext &) = delete;
    ClapHostContext &operator=(const ClapHostContext &) = delete;

    [[nodiscard]] const clap_host_t *handle() const noexcept { return &m_host; }

    // The thread the host considers "main". Set once at construction, which for
    // this subsystem is the thread that owns the call, because there is no
    // separate audio thread -- captured frames are handled on the thread that
    // captured them. Being able to say so accurately is the whole value of
    // implementing thread-check at all.
    [[nodiscard]] bool isMainThread() const noexcept
    {
        return std::this_thread::get_id() == m_mainThread;
    }

    // Set by the plugin calling latency.changed(). Read and cleared between
    // frames; never acted on inside one.
    [[nodiscard]] bool takeLatencyChanged() noexcept
    {
        const bool changed = m_latencyChanged;
        m_latencyChanged = false;
        return changed;
    }

    // Set by request_restart(). A restart means deactivate and reactivate,
    // which cannot happen inside a frame, so it is recorded and honoured
    // between them.
    [[nodiscard]] bool takeRestartRequested() noexcept
    {
        const bool requested = m_restartRequested;
        m_restartRequested = false;
        return requested;
    }

    [[nodiscard]] bool takeStateDirty() noexcept
    {
        const bool dirty = m_stateDirty;
        m_stateDirty = false;
        return dirty;
    }

    // What the plugin last said through clap.log. Kept only for diagnostics --
    // it is attacker-controlled text and never goes in front of a user.
    [[nodiscard]] const QString &lastLogMessage() const noexcept { return m_lastLogMessage; }

    // Recovers the context from the clap_host_t a plugin was handed. Public
    // because the extension vtables are free functions -- the ABI has no room
    // for a this pointer, so host_data is the only channel there is.
    [[nodiscard]] static ClapHostContext *self(const clap_host_t *host) noexcept;

    // Called from those vtables. Not part of the host's interface to the rest
    // of the subsystem, but they cannot be private for the same reason.
    void noteLogMessage(int severity, const char *message);
    void noteLatencyChanged() noexcept;
    void noteStateDirty() noexcept;

private:
    static const void *getExtension(const clap_host_t *host, const char *extensionId);
    static void requestRestart(const clap_host_t *host);
    static void requestProcess(const clap_host_t *host);
    static void requestCallback(const clap_host_t *host);

    clap_host_t m_host{};
    std::thread::id m_mainThread;
    bool m_latencyChanged = false;
    bool m_restartRequested = false;
    bool m_stateDirty = false;
    bool m_callbackRequested = false;
    QString m_lastLogMessage;
};

} // namespace OpenChat
