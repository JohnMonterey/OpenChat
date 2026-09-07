#include "effects/ClapHostContext.h"

#include "diagnostics/Logging.h"

namespace OpenChat {

namespace {

// Static extension tables. One instance each: the ABI hands the plugin a
// pointer that must stay valid for as long as the plugin might call it, and a
// stateless vtable can be shared by every host context in the process.

const clap_host_log_t hostLog = {
    [](const clap_host_t *host, clap_log_severity severity, const char *message) {
        if (ClapHostContext *context = ClapHostContext::self(host))
            context->noteLogMessage(severity, message);
    },
};

const clap_host_thread_check_t hostThreadCheck = {
    [](const clap_host_t *host) -> bool {
        const ClapHostContext *context = ClapHostContext::self(host);
        return context && context->isMainThread();
    },
    // There is no separate audio thread in this subsystem: a captured frame is
    // processed on the thread that captured it, which is the same thread the
    // chain was built on. Saying "this is the audio thread" whenever it is our
    // own thread is the honest answer to a question whose premise -- two
    // threads -- does not hold here.
    [](const clap_host_t *host) -> bool {
        const ClapHostContext *context = ClapHostContext::self(host);
        return context && context->isMainThread();
    },
};

const clap_host_latency_t hostLatency = {
    [](const clap_host_t *host) {
        if (ClapHostContext *context = ClapHostContext::self(host))
            context->noteLatencyChanged();
    },
};

const clap_host_state_t hostState = {
    [](const clap_host_t *host) {
        if (ClapHostContext *context = ClapHostContext::self(host))
            context->noteStateDirty();
    },
};

const clap_host_params_t hostParams = {
    // rescan: the plugin has rearranged its own parameter list. Nothing to do
    // mid-call -- the chain re-reads parameters when it is rebuilt -- but the
    // pointer must be callable.
    [](const clap_host_t *, clap_param_rescan_flags) {},
    [](const clap_host_t *, clap_id, clap_param_clear_flags) {},
    // request_flush is only meaningful while NOT processing, and this chain is
    // either processing or torn down. Parameters set while it is down are
    // applied when it comes back up.
    [](const clap_host_t *) {},
};

} // namespace

ClapHostContext::ClapHostContext()
    : m_mainThread(std::this_thread::get_id())
{
    m_host.clap_version = CLAP_VERSION;
    m_host.host_data = this;
    m_host.name = "OpenChat";
    m_host.vendor = "OpenChat";
    m_host.url = "https://github.com/JohnMonterey/OpenChat";
    m_host.version = "0.1.0";
    m_host.get_extension = &ClapHostContext::getExtension;
    m_host.request_restart = &ClapHostContext::requestRestart;
    m_host.request_process = &ClapHostContext::requestProcess;
    m_host.request_callback = &ClapHostContext::requestCallback;
}

ClapHostContext *ClapHostContext::self(const clap_host_t *host) noexcept
{
    return host ? static_cast<ClapHostContext *>(host->host_data) : nullptr;
}

const void *ClapHostContext::getExtension(const clap_host_t *host, const char *extensionId)
{
    if (!host || !extensionId)
        return nullptr;
    const QLatin1StringView id(extensionId);
    if (id == QLatin1StringView(CLAP_EXT_LOG))
        return &hostLog;
    if (id == QLatin1StringView(CLAP_EXT_THREAD_CHECK))
        return &hostThreadCheck;
    if (id == QLatin1StringView(CLAP_EXT_LATENCY))
        return &hostLatency;
    if (id == QLatin1StringView(CLAP_EXT_STATE))
        return &hostState;
    if (id == QLatin1StringView(CLAP_EXT_PARAMS))
        return &hostParams;
    // Everything else, including clap.gui: null means "this host does not do
    // that". A plugin that needs a window in a process with no display is a
    // plugin we cannot run anyway, and saying so by omission is better than
    // offering a GUI host that then refuses every call.
    return nullptr;
}

void ClapHostContext::requestRestart(const clap_host_t *host)
{
    if (ClapHostContext *context = self(host))
        context->m_restartRequested = true;
}

void ClapHostContext::requestProcess(const clap_host_t *host)
{
    // The chain calls process() on every frame unconditionally for as long as
    // it is running, so a plugin asking to be processed is already being
    // processed. Recorded rather than acted on.
    if (ClapHostContext *context = self(host))
        context->m_callbackRequested = true;
}

void ClapHostContext::requestCallback(const clap_host_t *host)
{
    if (ClapHostContext *context = self(host))
        context->m_callbackRequested = true;
}

void ClapHostContext::noteLogMessage(int severity, const char *message)
{
    if (!message)
        return;
    m_lastLogMessage = QString::fromUtf8(message);
    // Plugin output is somebody else's text: it goes to a logging category that
    // is off in a shipped build, and never to a user-facing string.
    if (severity >= CLAP_LOG_ERROR)
        qCWarning(effectsLog, "plugin: %s", message);
    else
        qCDebug(effectsLog, "plugin: %s", message);
}

void ClapHostContext::noteLatencyChanged() noexcept
{
    m_latencyChanged = true;
}

void ClapHostContext::noteStateDirty() noexcept
{
    m_stateDirty = true;
}

} // namespace OpenChat
