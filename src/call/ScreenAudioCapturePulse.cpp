// A screen share's sound on Linux, through PulseAudio's client API, which
// PipeWire's pulse server speaks as well.
//
// The desktop's own mix (the output's monitor) cannot be used: it contains
// OpenChat's playback, which is the other people in the call. So every
// application playing gets a monitor stream of its own — PulseAudio can record
// one playback stream on its own — except OpenChat's, and the framer sums
// them. Streams come and go as applications start and stop playing, followed
// through the server's change notifications.

#include "call/ScreenAudioCapture.h"

#include "diagnostics/BlackBox.h"

#include <QElapsedTimer>

#include <pulse/pulseaudio.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace OpenChat {

namespace {

constexpr const char *area = "screen share";
// Pieces the server hands over: one frame's worth, so each arrives on time
// for the frame it belongs to.
constexpr uint32_t fragmentBytes = ScreenAudioFormat::bytesPerFrame;
// A monitor that fails more often than this for one stream is given up on,
// rather than retried forever.
constexpr int maxAttempts = 3;
constexpr int connectTimeoutMs = 3000;

[[nodiscard]] qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class PulseScreenAudioCapture final : public ScreenAudioCapture
{
public:
    PulseScreenAudioCapture() = default;
    ~PulseScreenAudioCapture() override { stop(); }

    bool start(QString &failure) override;
    void stop() override;

    QString describe() const override
    {
        return QStringLiteral("every application except OpenChat, through the PulseAudio API");
    }

private:
    struct Monitor final {
        PulseScreenAudioCapture *owner = nullptr;
        pa_stream *stream = nullptr;
        uint32_t input = PA_INVALID_INDEX;
        int source = 0;
    };

    // All of these run on the mainloop thread, with its lock held.
    void consider(const pa_sink_input_info &info);
    void openMonitor(uint32_t input, const char *monitorSource);
    void closeMonitor(uint32_t input);
    void closeAll();
    void requery(uint32_t input);

    static void onContextState(pa_context *context, void *userdata);
    static void onSubscription(pa_context *context, pa_subscription_event_type_t type,
                               uint32_t index, void *userdata);
    static void onSinkInput(pa_context *context, const pa_sink_input_info *info, int eol,
                            void *userdata);
    static void onStreamState(pa_stream *stream, void *userdata);
    static void onStreamRead(pa_stream *stream, size_t bytes, void *userdata);

    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    std::map<uint32_t, Monitor *> m_monitors;
    std::map<uint32_t, int> m_attempts;
    int m_nextSource = 1;
    pid_t m_ownPid = getpid();

    ScreenAudioFramer m_framer;
    std::thread m_pump;
    std::atomic<bool> m_stopping{false};
    bool m_running = false;
};

// --- Starting and stopping ---------------------------------------------------------

bool PulseScreenAudioCapture::start(QString &failure)
{
    if (m_running)
        return true;
    BlackBox::Activity activity(area, "connecting to the sound server for the share's sound");
    m_loop = pa_threaded_mainloop_new();
    if (m_loop == nullptr) {
        failure = QStringLiteral("OpenChat could not start listening for sound.");
        return false;
    }
    pa_proplist *properties = pa_proplist_new();
    pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "OpenChat");
    pa_proplist_sets(properties, PA_PROP_APPLICATION_ID, "openchat.screen-sound");
    m_context = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(m_loop),
                                             "OpenChat screen sound", properties);
    pa_proplist_free(properties);
    if (m_context == nullptr) {
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
        failure = QStringLiteral("OpenChat could not start listening for sound.");
        return false;
    }
    pa_context_set_state_callback(m_context, &PulseScreenAudioCapture::onContextState, this);

    bool ready = false;
    pa_threaded_mainloop_lock(m_loop);
    const bool connecting = pa_threaded_mainloop_start(m_loop) == 0
        && pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) == 0;
    pa_threaded_mainloop_unlock(m_loop);
    if (connecting) {
        // Polled rather than waited on: a server that accepts the connection
        // and then never answers must not hold the share up forever.
        QElapsedTimer waited;
        waited.start();
        while (waited.elapsed() < connectTimeoutMs) {
            pa_threaded_mainloop_lock(m_loop);
            const pa_context_state_t state = pa_context_get_state(m_context);
            pa_threaded_mainloop_unlock(m_loop);
            if (state == PA_CONTEXT_READY) {
                ready = true;
                break;
            }
            if (!PA_CONTEXT_IS_GOOD(state))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    pa_threaded_mainloop_lock(m_loop);
    if (!ready) {
        pa_threaded_mainloop_unlock(m_loop);
        stop();
        failure = QStringLiteral("OpenChat could not reach the sound server (PulseAudio or "
                                 "PipeWire), so the share has no sound.");
        BlackBox::record(area, "the share's sound could not reach the sound server");
        return false;
    }
    pa_context_set_subscribe_callback(m_context, &PulseScreenAudioCapture::onSubscription, this);
    if (pa_operation *op = pa_context_subscribe(m_context, PA_SUBSCRIPTION_MASK_SINK_INPUT,
                                                nullptr, nullptr)) {
        pa_operation_unref(op);
    }
    if (pa_operation *op = pa_context_get_sink_input_info_list(
            m_context, &PulseScreenAudioCapture::onSinkInput, this)) {
        pa_operation_unref(op);
    }
    m_running = true;
    pa_threaded_mainloop_unlock(m_loop);

    m_stopping = false;
    m_pump = std::thread([this] {
        BlackBox::nameThread("screen sound");
        while (!m_stopping.load()) {
            m_framer.pump(nowMs(), [this](const StereoFrame &frame) {
                if (onFrame)
                    onFrame(frame);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    BlackBox::record(area, QStringLiteral("share's sound on: ") + describe());
    return true;
}

void PulseScreenAudioCapture::stop()
{
    // The pump goes first, so nothing is emitted from a half-closed capture.
    m_stopping = true;
    if (m_pump.joinable())
        m_pump.join();
    if (m_loop != nullptr) {
        pa_threaded_mainloop_lock(m_loop);
        closeAll();
        if (m_context != nullptr) {
            pa_context_set_subscribe_callback(m_context, nullptr, nullptr);
            pa_context_set_state_callback(m_context, nullptr, nullptr);
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        pa_threaded_mainloop_unlock(m_loop);
        pa_threaded_mainloop_stop(m_loop);
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
    }
    if (m_running)
        BlackBox::record(area, "share's sound off");
    m_running = false;
}

// --- Following the applications that play ------------------------------------------

void PulseScreenAudioCapture::onContextState(pa_context *context, void *userdata)
{
    auto *self = static_cast<PulseScreenAudioCapture *>(userdata);
    // A server that goes away mid-share takes every monitor with it; the
    // share carries on silent rather than failing.
    if (self->m_running && !PA_CONTEXT_IS_GOOD(pa_context_get_state(context))) {
        self->closeAll();
        BlackBox::record(area, "the sound server went away; the share continues without sound");
    }
}

void PulseScreenAudioCapture::onSubscription(pa_context *context,
                                             pa_subscription_event_type_t type, uint32_t index,
                                             void *userdata)
{
    auto *self = static_cast<PulseScreenAudioCapture *>(userdata);
    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT)
        return;
    const auto kind = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
        self->closeMonitor(index);
        self->m_attempts.erase(index);
        return;
    }
    // New, or changed (moved to another output, which ends our monitor of it).
    if (pa_operation *op = pa_context_get_sink_input_info(
            context, index, &PulseScreenAudioCapture::onSinkInput, self)) {
        pa_operation_unref(op);
    }
}

void PulseScreenAudioCapture::onSinkInput(pa_context *, const pa_sink_input_info *info, int eol,
                                          void *userdata)
{
    if (eol != 0 || info == nullptr)
        return;
    static_cast<PulseScreenAudioCapture *>(userdata)->consider(*info);
}

void PulseScreenAudioCapture::consider(const pa_sink_input_info &info)
{
    if (!m_running || m_monitors.count(info.index) != 0)
        return;
    // OpenChat's own playback — the call itself, its sounds — is exactly what
    // must never be sent back. Every stream of this process says so.
    if (const char *pid = pa_proplist_gets(info.proplist, PA_PROP_APPLICATION_PROCESS_ID)) {
        if (std::strtol(pid, nullptr, 10) == long(m_ownPid))
            return;
    }
    if (m_attempts[info.index] >= maxAttempts)
        return;
    // The monitor stream reads from the output's monitor source, which is
    // named on the output, so the output is looked up first.
    struct Lookup final {
        PulseScreenAudioCapture *self;
        uint32_t input;
    };
    auto *lookup = new Lookup{this, info.index};
    pa_operation *op = pa_context_get_sink_info_by_index(
        m_context, info.sink,
        [](pa_context *, const pa_sink_info *sink, int eol, void *userdata) {
            auto *lookup = static_cast<Lookup *>(userdata);
            if (eol != 0) {
                delete lookup;
                return;
            }
            if (sink != nullptr && sink->monitor_source_name != nullptr)
                lookup->self->openMonitor(lookup->input, sink->monitor_source_name);
        },
        lookup);
    if (op == nullptr)
        delete lookup;
    else
        pa_operation_unref(op);
}

void PulseScreenAudioCapture::openMonitor(uint32_t input, const char *monitorSource)
{
    if (!m_running || m_monitors.count(input) != 0)
        return;
    ++m_attempts[input];
    const pa_sample_spec spec{PA_SAMPLE_S16LE, uint32_t(ScreenAudioFormat::sampleRate),
                              uint8_t(ScreenAudioFormat::channels)};
    pa_stream *stream = pa_stream_new(m_context, "OpenChat screen sound", &spec, nullptr);
    if (stream == nullptr)
        return;
    auto *monitor = new Monitor{this, stream, input, m_nextSource++};
    pa_stream_set_monitor_stream(stream, input);
    pa_stream_set_state_callback(stream, &PulseScreenAudioCapture::onStreamState, monitor);
    pa_stream_set_read_callback(stream, &PulseScreenAudioCapture::onStreamRead, monitor);
    pa_buffer_attr attributes{};
    attributes.maxlength = uint32_t(-1);
    attributes.fragsize = fragmentBytes;
    const auto flags = pa_stream_flags_t(PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY);
    if (pa_stream_connect_record(stream, monitorSource, &attributes, flags) != 0) {
        pa_stream_set_state_callback(stream, nullptr, nullptr);
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_unref(stream);
        delete monitor;
        return;
    }
    m_monitors[input] = monitor;
    BlackBox::record(area, QStringLiteral("share's sound: %1 application stream(s)")
                               .arg(m_monitors.size()));
}

void PulseScreenAudioCapture::closeMonitor(uint32_t input)
{
    const auto found = m_monitors.find(input);
    if (found == m_monitors.end())
        return;
    Monitor *monitor = found->second;
    m_monitors.erase(found);
    pa_stream_set_state_callback(monitor->stream, nullptr, nullptr);
    pa_stream_set_read_callback(monitor->stream, nullptr, nullptr);
    pa_stream_disconnect(monitor->stream);
    pa_stream_unref(monitor->stream);
    m_framer.removeSource(monitor->source);
    delete monitor;
}

void PulseScreenAudioCapture::closeAll()
{
    while (!m_monitors.empty())
        closeMonitor(m_monitors.begin()->first);
    m_attempts.clear();
}

void PulseScreenAudioCapture::requery(uint32_t input)
{
    if (m_context == nullptr)
        return;
    if (pa_operation *op = pa_context_get_sink_input_info(
            m_context, input, &PulseScreenAudioCapture::onSinkInput, this)) {
        pa_operation_unref(op);
    }
}

void PulseScreenAudioCapture::onStreamState(pa_stream *stream, void *userdata)
{
    auto *monitor = static_cast<Monitor *>(userdata);
    const pa_stream_state_t state = pa_stream_get_state(stream);
    if (state != PA_STREAM_FAILED && state != PA_STREAM_TERMINATED)
        return;
    // The application stopped, or moved to another output, which ends a
    // monitor that may not move with it. Closed here (libpulse holds its own
    // reference for the length of this callback) and asked about again: if
    // the stream still exists, it gets a monitor on its new output.
    PulseScreenAudioCapture *self = monitor->owner;
    const uint32_t input = monitor->input;
    self->closeMonitor(input);
    self->requery(input);
}

void PulseScreenAudioCapture::onStreamRead(pa_stream *stream, size_t, void *userdata)
{
    auto *monitor = static_cast<Monitor *>(userdata);
    const void *data = nullptr;
    size_t bytes = 0;
    while (pa_stream_readable_size(stream) > 0) {
        if (pa_stream_peek(stream, &data, &bytes) != 0)
            return;
        if (bytes == 0)
            return;
        const auto frames = qsizetype(bytes / (ScreenAudioFormat::channels * ScreenAudioFormat::bytesPerSample));
        // A hole in the stream is reported as data with no pointer.
        if (data != nullptr)
            monitor->owner->m_framer.push(monitor->source, static_cast<const qint16 *>(data), frames);
        else
            monitor->owner->m_framer.pushSilence(monitor->source, frames);
        pa_stream_drop(stream);
    }
}

} // namespace

namespace ScreenAudioCapturePlatform {

bool isSupported()
{
    return true;
}

QString unsupportedReason()
{
    return {};
}

std::unique_ptr<ScreenAudioCapture> create(const ScreenAudioTarget &)
{
    // Which application owns a window is not something a Wayland client can
    // learn, so a window share carries the same sound as a screen share.
    return std::make_unique<PulseScreenAudioCapture>();
}

QString backendName()
{
    return QStringLiteral("PulseAudio API");
}

} // namespace ScreenAudioCapturePlatform

} // namespace OpenChat
