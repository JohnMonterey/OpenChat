// A screen share's sound on Windows: WASAPI process loopback.
//
// Windows 10 2004 added a loopback that follows processes rather than a sound
// card: everything a process tree plays, or everything except what it plays.
// A screen share excludes OpenChat's own tree, so the call itself is never
// captured and sent back; a window share includes only the tree of the
// process that owns the window (a browser's audio runs in a child process,
// which the tree covers).
//
// There is also an endpoint loopback — the whole output, OpenChat included —
// used only when OPENCHAT_SCREEN_AUDIO=endpoint asks for it. It exists so the
// capture loop can be exercised where process loopback does not exist (Wine);
// it would echo the call back, so nothing chooses it on its own.

#ifndef NOMINMAX
#    define NOMINMAX
#endif

#include "call/ScreenAudioCapture.h"

#include "diagnostics/BlackBox.h"

#include <QOperatingSystemVersion>
#include <QtGlobal>

#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objidl.h>
#include <propidl.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>

namespace OpenChat {

// From audioclientactivationparams.h, which mingw-w64 does not ship. Plain
// data with the SDK's layout; named rather than anonymous so nothing here can
// ever be mistaken for a class with no implementations (see
// NativeScreenCaptureWin.cpp for what that costs).
namespace WasapiProcessLoopback {

enum class ActivationType : int { Default = 0, ProcessLoopback = 1 };
enum class LoopbackMode : int { IncludeTargetProcessTree = 0, ExcludeTargetProcessTree = 1 };

struct Params final {
    DWORD targetProcessId;
    LoopbackMode mode;
};

struct ActivationParams final {
    ActivationType type;
    Params processLoopback;
};
static_assert(sizeof(ActivationParams) == 12, "must match AUDIOCLIENT_ACTIVATION_PARAMS");

constexpr wchar_t device[] = L"VAD\\Process_Loopback";

} // namespace WasapiProcessLoopback

// Format tags and the float sub-format, spelled out rather than taken from
// mmreg.h/ksmedia.h, whose GUID needs a library of its own to link.
namespace WaveFormat {
constexpr WORD pcm = 0x0001;
constexpr WORD ieeeFloat = 0x0003;
constexpr WORD extensible = 0xFFFE;
constexpr GUID floatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
} // namespace WaveFormat

namespace {

using Microsoft::WRL::ComPtr;

constexpr const char *area = "screen share";
// 200 ms of buffer on the system side; we drain it every few milliseconds.
constexpr REFERENCE_TIME bufferDuration = 2'000'000;
constexpr DWORD activationTimeoutMs = 3000;
constexpr int startTimeoutMs = 5000;

using ActivateFunction = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *,
                                           IActivateAudioInterfaceCompletionHandler *,
                                           IActivateAudioInterfaceAsyncOperation **);

[[nodiscard]] ActivateFunction activateFunction()
{
    static const ActivateFunction function = [] {
        HMODULE module = GetModuleHandleW(L"mmdevapi.dll");
        if (module == nullptr)
            module = LoadLibraryW(L"mmdevapi.dll");
        return module == nullptr ? nullptr
                                 : reinterpret_cast<ActivateFunction>(reinterpret_cast<void *>(
                                       GetProcAddress(module, "ActivateAudioInterfaceAsync")));
    }();
    return function;
}

[[nodiscard]] qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// The asynchronous activation's callback. Windows calls it on a thread of its
// own, which is why it must say it is agile.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    ActivationHandler()
        : m_done(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    ActivationHandler(const ActivationHandler &) = delete;
    ActivationHandler &operator=(const ActivationHandler &) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (object == nullptr)
            return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
        } else if (iid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject *>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++m_references); }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const long left = --m_references;
        if (left == 0)
            delete this;
        return ULONG(left);
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation) override
    {
        HRESULT activated = E_FAIL;
        IUnknown *unknown = nullptr;
        const HRESULT hr = operation->GetActivateResult(&activated, &unknown);
        {
            const std::lock_guard lock(m_mutex);
            m_result = FAILED(hr) ? hr : activated;
            if (SUCCEEDED(m_result) && unknown != nullptr)
                m_result = unknown->QueryInterface(__uuidof(IAudioClient),
                                                   reinterpret_cast<void **>(m_client.ReleaseAndGetAddressOf()));
        }
        if (unknown != nullptr)
            unknown->Release();
        SetEvent(m_done);
        return S_OK;
    }

    // Waits for the activation. False on timeout; otherwise the result.
    [[nodiscard]] bool wait(DWORD timeoutMs, HRESULT &result, ComPtr<IAudioClient> &client)
    {
        if (m_done == nullptr || WaitForSingleObject(m_done, timeoutMs) != WAIT_OBJECT_0)
            return false;
        const std::lock_guard lock(m_mutex);
        result = m_result;
        client = m_client;
        return true;
    }

private:
    ~ActivationHandler()
    {
        if (m_done != nullptr)
            CloseHandle(m_done);
    }

    std::atomic<long> m_references{1};
    HANDLE m_done = nullptr;
    std::mutex m_mutex;
    HRESULT m_result = E_PENDING;
    ComPtr<IAudioClient> m_client;
};

[[nodiscard]] bool processLoopbackSupported()
{
    // Process loopback arrived in Windows 10 2004, build 19041.
    return QOperatingSystemVersion::current()
               >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 19041)
        && activateFunction() != nullptr;
}

// True for the executable name of `processId`, case-insensitively.
[[nodiscard]] bool processImageIs(DWORD processId, const wchar_t *name)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
        return false;
    wchar_t path[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    const bool known = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
    CloseHandle(process);
    if (!known)
        return false;
    const wchar_t *file = std::wcsrchr(path, L'\\');
    return _wcsicmp(file != nullptr ? file + 1 : path, name) == 0;
}

class WasapiScreenAudioCapture final : public ScreenAudioCapture
{
public:
    enum class Mode { AllButOpenChat, OneProcess, Endpoint };

    WasapiScreenAudioCapture(Mode mode, DWORD processId)
        : m_mode(mode)
        , m_processId(processId)
    {
    }

    ~WasapiScreenAudioCapture() override { stop(); }

    bool start(QString &failure) override;
    void stop() override;

    QString describe() const override
    {
        switch (m_mode) {
        case Mode::AllButOpenChat:
            return QStringLiteral("every application except OpenChat, through WASAPI process loopback");
        case Mode::OneProcess:
            return QStringLiteral("the shared window's application, through WASAPI process loopback");
        case Mode::Endpoint:
            return QStringLiteral("the whole default output, OpenChat included, through WASAPI "
                                  "endpoint loopback (testing only)");
        }
        return {};
    }

private:
    void run();
    // Opens the client for the current mode. On failure, a sentence for the user.
    [[nodiscard]] bool open(QString &failure);
    void drain();
    void finishStartup(bool ok, const QString &failure);

    const Mode m_mode;
    const DWORD m_processId;
    std::thread m_thread;
    std::atomic<bool> m_stopping{false};
    ScreenAudioFramer m_framer;

    // The capture thread's own objects.
    ComPtr<IAudioClient> m_client;
    ComPtr<IAudioCaptureClient> m_capture;
    HANDLE m_event = nullptr;
    std::unique_ptr<StereoConverter> m_converter;
    std::vector<qint16> m_converted;

    std::mutex m_startMutex;
    std::condition_variable m_startCondition;
    bool m_startDone = false;
    bool m_startOk = false;
    QString m_startFailure;
};

bool WasapiScreenAudioCapture::start(QString &failure)
{
    if (m_thread.joinable())
        return true;
    m_stopping = false;
    {
        const std::lock_guard lock(m_startMutex);
        m_startDone = false;
        m_startOk = false;
        m_startFailure.clear();
    }
    m_thread = std::thread([this] { run(); });
    std::unique_lock lock(m_startMutex);
    const bool answered = m_startCondition.wait_for(lock, std::chrono::milliseconds(startTimeoutMs),
                                                    [this] { return m_startDone; });
    const bool ok = answered && m_startOk;
    if (!ok)
        failure = answered ? m_startFailure
                           : QStringLiteral("Windows did not start capturing sound in time.");
    lock.unlock();
    if (!ok) {
        stop();
        BlackBox::record(area, QStringLiteral("share's sound failed to start: ") + failure);
        return false;
    }
    BlackBox::record(area, QStringLiteral("share's sound on: ") + describe());
    return true;
}

void WasapiScreenAudioCapture::stop()
{
    m_stopping = true;
    if (m_thread.joinable()) {
        m_thread.join();
        BlackBox::record(area, "share's sound off");
    }
}

void WasapiScreenAudioCapture::finishStartup(bool ok, const QString &failure)
{
    {
        const std::lock_guard lock(m_startMutex);
        m_startDone = true;
        m_startOk = ok;
        m_startFailure = failure;
    }
    m_startCondition.notify_all();
}

bool WasapiScreenAudioCapture::open(QString &failure)
{
    HRESULT hr = S_OK;
    if (m_mode == Mode::Endpoint) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(enumerator.GetAddressOf()));
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(hr))
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(m_client.ReleaseAndGetAddressOf()));
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(hr))
            hr = m_client->GetMixFormat(&mix);
        if (SUCCEEDED(hr)) {
            // The mix format is float nearly everywhere; 16-bit is handled too.
            bool isFloat = mix->wFormatTag == WaveFormat::ieeeFloat;
            if (mix->wFormatTag == WaveFormat::extensible && mix->cbSize >= 22) {
                const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix);
                isFloat = extensible->SubFormat == WaveFormat::floatSubtype;
            }
            const bool usable = (isFloat && mix->wBitsPerSample == 32) || (!isFloat && mix->wBitsPerSample == 16);
            if (!usable) {
                hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            } else {
                m_converter = std::make_unique<StereoConverter>(
                    int(mix->nSamplesPerSec), int(mix->nChannels),
                    isFloat ? StereoConverter::Sample::Float32 : StereoConverter::Sample::Int16);
                hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          bufferDuration, 0, mix, nullptr);
            }
            CoTaskMemFree(mix);
        }
    } else {
        namespace Loopback = WasapiProcessLoopback;
        Loopback::ActivationParams params{};
        params.type = Loopback::ActivationType::ProcessLoopback;
        params.processLoopback.targetProcessId =
            m_mode == Mode::OneProcess ? m_processId : GetCurrentProcessId();
        params.processLoopback.mode = m_mode == Mode::OneProcess
            ? Loopback::LoopbackMode::IncludeTargetProcessTree
            : Loopback::LoopbackMode::ExcludeTargetProcessTree;
        PROPVARIANT variant{};
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof(params);
        variant.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

        auto *handler = new ActivationHandler;
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        hr = activateFunction()(Loopback::device, __uuidof(IAudioClient), &variant, handler,
                                operation.GetAddressOf());
        if (SUCCEEDED(hr)) {
            HRESULT activated = E_FAIL;
            if (!handler->wait(activationTimeoutMs, activated, m_client))
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            else
                hr = activated;
        }
        handler->Release();
        if (SUCCEEDED(hr) && !m_client)
            hr = E_NOINTERFACE;

        // Process loopback does not report a mix format: it converts to
        // whatever is asked for, so the call's own format is asked for.
        WAVEFORMATEX format{};
        format.wFormatTag = WaveFormat::pcm;
        format.nChannels = WORD(ScreenAudioFormat::channels);
        format.nSamplesPerSec = DWORD(ScreenAudioFormat::sampleRate);
        format.wBitsPerSample = WORD(ScreenAudioFormat::bytesPerSample * 8);
        format.nBlockAlign = WORD(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (SUCCEEDED(hr)) {
            hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                          | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                      bufferDuration, 0, &format, nullptr);
        }
        if (SUCCEEDED(hr)) {
            m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = m_event == nullptr ? HRESULT_FROM_WIN32(GetLastError())
                                    : m_client->SetEventHandle(m_event);
        }
    }
    if (SUCCEEDED(hr))
        hr = m_client->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void **>(m_capture.ReleaseAndGetAddressOf()));
    if (SUCCEEDED(hr))
        hr = m_client->Start();
    if (FAILED(hr)) {
        failure = QStringLiteral("Windows could not start capturing the sound to share (%1).")
                      .arg(hresultText(hr));
        return false;
    }
    return true;
}

void WasapiScreenAudioCapture::drain()
{
    UINT32 waiting = 0;
    while (SUCCEEDED(m_capture->GetNextPacketSize(&waiting)) && waiting > 0) {
        BYTE *data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            return;
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
            m_framer.pushSilence(0, qsizetype(frames));
        } else if (m_converter) {
            m_converted.clear();
            m_converter->convert(data, qsizetype(frames), m_converted);
            m_framer.push(0, m_converted.data(), qsizetype(m_converted.size() / 2));
        } else {
            m_framer.push(0, reinterpret_cast<const qint16 *>(data), qsizetype(frames));
        }
        m_capture->ReleaseBuffer(frames);
    }
}

void WasapiScreenAudioCapture::run()
{
    BlackBox::nameThread("screen sound");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        BlackBox::Activity activity(area, "starting to capture the share's sound");
        QString failure;
        if (!open(failure)) {
            finishStartup(false, failure);
            m_capture.Reset();
            m_client.Reset();
            if (m_event != nullptr) {
                CloseHandle(m_event);
                m_event = nullptr;
            }
            if (SUCCEEDED(com))
                CoUninitialize();
            return;
        }
    }
    finishStartup(true, {});

    while (!m_stopping.load()) {
        // The event, where there is one, says a packet is ready; the timeout
        // keeps the frames coming while nothing plays.
        if (m_event != nullptr)
            WaitForSingleObject(m_event, 5);
        else
            Sleep(5);
        drain();
        m_framer.pump(nowMs(), [this](const StereoFrame &frame) {
            if (onFrame)
                onFrame(frame);
        });
    }

    m_client->Stop();
    m_capture.Reset();
    m_client.Reset();
    if (m_event != nullptr) {
        CloseHandle(m_event);
        m_event = nullptr;
    }
    if (SUCCEEDED(com))
        CoUninitialize();
}

} // namespace

namespace ScreenAudioCapturePlatform {

namespace {

[[nodiscard]] bool endpointRequested()
{
    return qEnvironmentVariable("OPENCHAT_SCREEN_AUDIO").trimmed().toLower() == QStringLiteral("endpoint");
}

} // namespace

bool isSupported()
{
    return endpointRequested() || processLoopbackSupported();
}

QString unsupportedReason()
{
    if (isSupported())
        return {};
    return QStringLiteral("Sharing sound needs Windows 10 version 2004 or later.");
}

std::unique_ptr<ScreenAudioCapture> create(const ScreenAudioTarget &target)
{
    using Capture = WasapiScreenAudioCapture;
    if (endpointRequested())
        return std::make_unique<Capture>(Capture::Mode::Endpoint, 0);
    if (!processLoopbackSupported())
        return nullptr;
    if (target.window && target.nativeWindow != 0) {
        DWORD owner = 0;
        GetWindowThreadProcessId(reinterpret_cast<HWND>(target.nativeWindow), &owner);
        // A window of OpenChat itself shares everything but OpenChat, like a
        // screen: its own output is the call. A Store app's window belongs to
        // the frame host, not to the app that plays, so it does the same.
        if (owner != 0 && owner != GetCurrentProcessId()
            && !processImageIs(owner, L"ApplicationFrameHost.exe")) {
            return std::make_unique<Capture>(Capture::Mode::OneProcess, owner);
        }
    }
    return std::make_unique<Capture>(Capture::Mode::AllButOpenChat, 0);
}

QString backendName()
{
    return endpointRequested() ? QStringLiteral("WASAPI endpoint loopback")
                               : QStringLiteral("WASAPI process loopback");
}

} // namespace ScreenAudioCapturePlatform

} // namespace OpenChat
