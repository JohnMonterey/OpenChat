#include "call/NativeScreenCapture.h"

#include "call/QtScreenCapture.h"
#include "diagnostics/BlackBox.h"

#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QRect>
#include <QScopeGuard>
#include <QScreen>
#include <QTransform>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

// Windows.Graphics.Capture (single windows, and screens Desktop Duplication
// refuses) is reached through C++/WinRT, which ships with the Windows SDK and
// with recent mingw-w64. Without it only whole screens can be shared. The
// build links the WinRT libraries under the same condition.
#if !defined(OPENCHAT_HAVE_WGC)
#    if __has_include(<winrt/Windows.Graphics.Capture.h>)
#        define OPENCHAT_HAVE_WGC 1
#    else
#        define OPENCHAT_HAVE_WGC 0
#    endif
#endif

#if OPENCHAT_HAVE_WGC
#    include <winrt/Windows.Foundation.h>
#    include <winrt/Windows.Graphics.Capture.h>
#    include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#    include <winrt/Windows.Graphics.DirectX.h>
#    include <winrt/Windows.Graphics.h>
#endif

namespace OpenChat {

#if OPENCHAT_HAVE_WGC
// The two classic COM interfaces that join WinRT capture to Direct3D, from
// windows.graphics.capture.interop.h and
// windows.graphics.directx.direct3d11.interop.h. Declared here rather than
// included: mingw-w64 lacks the second, and the first drags in ABI headers that
// redefine one another under GCC. In a namespace of their own, so a toolchain
// that has them cannot collide.
//
// NOT in the anonymous namespace below, and that matters. For a class with
// internal linkage GCC assumes it can see every class derived from it. These
// have none here — the objects behind them come from Windows — so it concluded
// that the pure virtual declaration was the only possible target and compiled
// every call through them as a call to __cxa_pure_virtual, which this build
// resolves to address 0: sharing a window jumped straight into nothing.
// tools/windows/check-branches.sh catches that kind of call in the linked
// program.
namespace WinRtInterop {

struct CaptureItemInterop : ::IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND window, REFIID iid, void **result) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR monitor, REFIID iid,
                                                       void **result) = 0;
};

struct DxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **object) = 0;
};

} // namespace WinRtInterop
#endif

namespace {

using Microsoft::WRL::ComPtr;

constexpr const char *area = "screen share";

// How often a lost duplication is reopened, and how long it may stay lost —
// the lock screen, a UAC prompt, a display mode change — before the share is
// ended rather than left showing its last picture.
constexpr qint64 reopenIntervalMs = 250;
constexpr qint64 lostGiveUpMs = 20000;
// The GDI fallback's ceiling: ten whole-desktop copies a second.
constexpr qint64 gdiCopyIntervalMs = 100;

enum class ScreenApi { DesktopDuplication, GraphicsCapture, Gdi };

[[nodiscard]] QString hresultCode(HRESULT hr)
{
    return QStringLiteral("0x")
        + QString::number(quint32(hr), 16).toUpper().rightJustified(8, QLatin1Char('0'));
}

// The code, and the system's own words for it where it has any. Both go into
// the message the user sees, because that message is what a tester reports.
[[nodiscard]] QString describeHresult(HRESULT hr)
{
    QString text = hresultCode(hr);
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, DWORD(hr), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length > 0 && buffer != nullptr)
        text += QStringLiteral(": ") + QString::fromWCharArray(buffer, int(length)).trimmed();
    if (buffer != nullptr)
        LocalFree(buffer);
    return text;
}

[[nodiscard]] ScreenApi preferredScreenApi()
{
    const QString choice = qEnvironmentVariable("OPENCHAT_SCREEN_CAPTURE").trimmed().toLower();
    if (choice == QStringLiteral("wgc"))
        return ScreenApi::GraphicsCapture;
    if (choice == QStringLiteral("gdi"))
        return ScreenApi::Gdi;
    return ScreenApi::DesktopDuplication;
}

// --- Enumeration ------------------------------------------------------------

struct DxgiOutput final {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    QString adapterName;
    bool primary = false;
};

[[nodiscard]] std::vector<DxgiOutput> enumerateOutputs(HRESULT *error = nullptr)
{
    std::vector<DxgiOutput> outputs;
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void **>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        if (error != nullptr)
            *error = hr;
        return outputs;
    }
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr) || !adapter)
            break;
        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr) || !output)
                break;
            DxgiOutput entry;
            if (FAILED(output->GetDesc(&entry.desc)) || !entry.desc.AttachedToDesktop)
                continue;
            entry.adapter = adapter;
            entry.output = output;
            entry.adapterName = QString::fromWCharArray(adapterDesc.Description);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            entry.primary = GetMonitorInfoW(entry.desc.Monitor, &info) != FALSE
                && (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            outputs.push_back(std::move(entry));
        }
    }
    return outputs;
}

[[nodiscard]] QRect desktopRect(const DXGI_OUTPUT_DESC &desc)
{
    const RECT &r = desc.DesktopCoordinates;
    return QRect(QPoint(r.left, r.top), QSize(r.right - r.left, r.bottom - r.top));
}

// Qt names a Windows screen after the same GDI device DXGI reports; the
// geometry test covers any Qt that does not. Qt keeps a screen's top-left in
// native pixels and scales only its size.
[[nodiscard]] QScreen *matchQtScreen(const QString &deviceName, const QRect &nativeRect)
{
    if (qGuiApp == nullptr)
        return nullptr;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen != nullptr && screen->name() == deviceName)
            return screen;
    }
    for (QScreen *screen : screens) {
        if (screen == nullptr)
            continue;
        const QRect geometry = screen->geometry();
        const double ratio = screen->devicePixelRatio();
        if (geometry.topLeft() == nativeRect.topLeft()
            && qAbs(geometry.width() * ratio - nativeRect.width()) <= 2
            && qAbs(geometry.height() * ratio - nativeRect.height()) <= 2) {
            return screen;
        }
    }
    return nullptr;
}

[[nodiscard]] int displayNumber(const QString &deviceName, int fallback)
{
    // "\\.\DISPLAY3" -> 3, which is the number Windows' own display settings use.
    int start = int(deviceName.size());
    while (start > 0 && deviceName.at(start - 1).isDigit())
        --start;
    bool ok = false;
    const int number = deviceName.mid(start).toInt(&ok);
    return ok && number > 0 ? number : fallback;
}

[[nodiscard]] QString processNameOf(HWND window)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr)
        return {};
    wchar_t path[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    QString name;
    if (QueryFullProcessImageNameW(process, 0, path, &length))
        name = QFileInfo(QString::fromWCharArray(path, int(length))).fileName();
    CloseHandle(process);
    return name;
}

struct WindowCandidate final {
    HWND handle = nullptr;
    QString title;
    QSize size;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM parameter)
{
    auto *windows = reinterpret_cast<std::vector<WindowCandidate> *>(parameter);
    // What a person would call "a window": visible, not minimised (a
    // minimised window produces no frames to share), top-level rather than
    // owned by another, and not a tool palette.
    if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER) != nullptr)
        return TRUE;
    if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0)
        return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    // OpenChat's own window would only show the share inside the share.
    if (processId == GetCurrentProcessId())
        return TRUE;
    // Suspended store apps and windows on other virtual desktops are cloaked:
    // present, "visible", and not on screen.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked) {
        return TRUE;
    }
    wchar_t className[128] = {};
    GetClassNameW(window, className, int(std::size(className)));
    static const wchar_t *const shellClasses[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"Windows.UI.Core.CoreWindow",
    };
    for (const wchar_t *shellClass : shellClasses) {
        if (std::wcscmp(className, shellClass) == 0)
            return TRUE;
    }
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
        return TRUE;
    std::wstring title(size_t(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    if (copied <= 0)
        return TRUE;
    RECT rect{};
    GetWindowRect(window, &rect);
    const QSize size(rect.right - rect.left, rect.bottom - rect.top);
    if (size.width() < 32 || size.height() < 32)
        return TRUE;
    windows->push_back({window, QString::fromWCharArray(title.c_str(), copied), size});
    return TRUE;
}

// --- GPU read-back shared by both APIs ---------------------------------------

// One CPU-readable copy of the newest frame. It is what lets a still screen's
// last picture be handed to the encoder again without asking the API for a
// new one, and it is the only frame-sized buffer either capture holds.
class StagingFrame final
{
public:
    [[nodiscard]] bool ensure(ID3D11Device *device, UINT width, UINT height, DXGI_FORMAT format,
                              bool writable)
    {
        if (m_texture) {
            D3D11_TEXTURE2D_DESC current{};
            m_texture->GetDesc(&current);
            if (current.Width == width && current.Height == height && current.Format == format)
                return true;
            m_texture.Reset();
            m_valid = false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | (writable ? D3D11_CPU_ACCESS_WRITE : 0);
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, m_texture.GetAddressOf()));
    }

    [[nodiscard]] ID3D11Texture2D *texture() const { return m_texture.Get(); }
    [[nodiscard]] bool isValid() const { return m_texture && m_valid; }
    void markValid() { m_valid = true; }
    void reset()
    {
        m_texture.Reset();
        m_valid = false;
    }

private:
    ComPtr<ID3D11Texture2D> m_texture;
    bool m_valid = false;
};

[[nodiscard]] HRESULT createDevice(IDXGIAdapter *adapter, ComPtr<ID3D11Device> &device,
                                   ComPtr<ID3D11DeviceContext> &context)
{
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    const D3D_DRIVER_TYPE type = adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN
                                                    : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                                   device.ReleaseAndGetAddressOf(), nullptr,
                                   context.ReleaseAndGetAddressOf());
    // A Direct3D 11.0 runtime does not know 11.1 and refuses the whole list.
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(adapter, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels + 1, UINT(std::size(levels) - 1), D3D11_SDK_VERSION,
                               device.ReleaseAndGetAddressOf(), nullptr,
                               context.ReleaseAndGetAddressOf());
    }
    return hr;
}

// --- Windows.Graphics.Capture ------------------------------------------------

#if OPENCHAT_HAVE_WGC

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

using WinRtInterop::CaptureItemInterop;
using WinRtInterop::DxgiInterfaceAccess;

constexpr GUID iidDxgiInterfaceAccess = {
    0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
constexpr GUID iidGraphicsCaptureItemInterop = {
    0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};

// d3d11.dll has exported this since Windows 10; looked up rather than linked so
// an older system simply reports window capture as unavailable.
using CreateWinRtDeviceFunction = HRESULT(WINAPI *)(IDXGIDevice *, void **);

[[nodiscard]] CreateWinRtDeviceFunction createWinRtDeviceFunction()
{
    static const CreateWinRtDeviceFunction function = [] {
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11 == nullptr)
            d3d11 = LoadLibraryW(L"d3d11.dll");
        return d3d11 == nullptr ? nullptr
                                : reinterpret_cast<CreateWinRtDeviceFunction>(reinterpret_cast<void *>(
                                      GetProcAddress(d3d11, "CreateDirect3D11DeviceFromDXGIDevice")));
    }();
    return function;
}

void ensureApartment()
{
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
        // Already initialised, possibly in the other mode: WinRT still works.
    }
}

[[nodiscard]] QString winrtErrorText(const winrt::hresult_error &error)
{
    const QString message = QString::fromWCharArray(error.message().c_str()).trimmed();
    return hresultCode(error.code()) + (message.isEmpty() ? QString() : QStringLiteral(": ") + message);
}

[[nodiscard]] bool graphicsCaptureSupported()
{
    static const bool supported = [] {
        try {
            ensureApartment();
            if (createWinRtDeviceFunction() == nullptr
                || !wgc::GraphicsCaptureSession::IsSupported()) {
                return false;
            }
            // The interop factory is what reaches a window by its HWND; it
            // arrived in Windows 10 1903, after the API itself.
            auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
            ::IUnknown *unknown = static_cast<::IUnknown *>(winrt::get_abi(factory));
            CaptureItemInterop *interop = nullptr;
            if (FAILED(unknown->QueryInterface(iidGraphicsCaptureItemInterop,
                                               reinterpret_cast<void **>(&interop)))
                || interop == nullptr) {
                return false;
            }
            interop->Release();
            return true;
        } catch (...) {
            return false;
        }
    }();
    return supported;
}

class GraphicsCapture final : public NativeScreenCapture
{
public:
    GraphicsCapture(HWND window, HMONITOR monitor, QString subject)
        : m_window(window)
        , m_monitor(monitor)
        , m_subject(std::move(subject))
    {
    }

    ~GraphicsCapture() override { close(); }

    QString describe() const override
    {
        QString text = QStringLiteral("Windows.Graphics.Capture, ") + m_subject;
        if (m_size.Width > 0)
            text += QStringLiteral(", %1x%2").arg(m_size.Width).arg(m_size.Height);
        if (m_software)
            text += QStringLiteral(", software rendering");
        return text;
    }

    bool start(Failure &failure) override
    {
        BlackBox::Activity activity(area, "starting Windows.Graphics.Capture");
        try {
            ensureApartment();
            if (!graphicsCaptureSupported()) {
                failure.message = QStringLiteral(
                    "This version of Windows cannot capture single windows. Windows 10 "
                    "version 1903 or later is needed; whole screens can still be shared.");
                failure.permanent = true;
                return false;
            }
            HRESULT hr = createDevice(nullptr, m_device, m_context);
            if (FAILED(hr)) {
                // No usable GPU (a virtual machine, a broken driver): the
                // software rasteriser still composes a picture, just slower.
                static const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_1;
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
                                       D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(),
                                       nullptr, m_context.ReleaseAndGetAddressOf());
                m_software = SUCCEEDED(hr);
            }
            if (FAILED(hr)) {
                failure.message = QStringLiteral("OpenChat could not use the graphics card to "
                                                 "capture the screen (%1).")
                                      .arg(describeHresult(hr));
                return false;
            }
            ComPtr<IDXGIDevice> dxgiDevice;
            winrt::check_hresult(m_device.As(&dxgiDevice));
            ::IUnknown *inspectable = nullptr;
            winrt::check_hresult(createWinRtDeviceFunction()(
                dxgiDevice.Get(), reinterpret_cast<void **>(&inspectable)));
            const auto releaseInspectable = qScopeGuard([inspectable] { inspectable->Release(); });
            winrt::check_hresult(inspectable->QueryInterface(
                reinterpret_cast<const GUID &>(winrt::guid_of<wd3d::IDirect3DDevice>()),
                winrt::put_abi(m_winrtDevice)));

            auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
            CaptureItemInterop *interop = nullptr;
            winrt::check_hresult(static_cast<::IUnknown *>(winrt::get_abi(factory))
                                     ->QueryInterface(iidGraphicsCaptureItemInterop,
                                                      reinterpret_cast<void **>(&interop)));
            const auto releaseInterop = qScopeGuard([interop] { interop->Release(); });
            const GUID &itemIid = reinterpret_cast<const GUID &>(
                winrt::guid_of<wgc::GraphicsCaptureItem>());
            hr = m_window != nullptr
                ? interop->CreateForWindow(m_window, itemIid, winrt::put_abi(m_item))
                : interop->CreateForMonitor(m_monitor, itemIid, winrt::put_abi(m_item));
            if (FAILED(hr) || !m_item) {
                failure.message = m_window != nullptr
                    ? QStringLiteral("That window cannot be captured (%1). Some protected or "
                                     "elevated windows refuse screen capture.")
                          .arg(describeHresult(hr))
                    : QStringLiteral("That display cannot be captured (%1).")
                          .arg(describeHresult(hr));
                close();
                return false;
            }
            m_size = m_item.Size();
            m_size.Width = std::max(m_size.Width, 1);
            m_size.Height = std::max(m_size.Height, 1);
            m_pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_size);
            m_session = m_pool.CreateCaptureSession(m_item);
            // Both are newer than the API itself: the cursor switch arrived in
            // Windows 10 2004 and the border switch in Windows 11. Where they
            // are missing the defaults (cursor shown, yellow border) apply.
            try {
                m_session.IsCursorCaptureEnabled(true);
            } catch (...) {
            }
            try {
                m_session.IsBorderRequired(false);
            } catch (...) {
            }
            std::shared_ptr<std::atomic<bool>> closed = m_closed;
            m_closedRevoker = m_item.Closed(
                winrt::auto_revoke,
                [closed](const wgc::GraphicsCaptureItem &, const winrt::Windows::Foundation::IInspectable &) {
                    closed->store(true);
                });
            m_session.StartCapture();
            return true;
        } catch (const winrt::hresult_error &error) {
            failure.message = QStringLiteral("Windows could not start capturing %1 (%2).")
                                  .arg(m_subject, winrtErrorText(error));
        } catch (const std::exception &error) {
            failure.message = QStringLiteral("Windows could not start capturing %1 (%2).")
                                  .arg(m_subject, QString::fromLocal8Bit(error.what()));
        }
        close();
        return false;
    }

    Pull pull(bool redeliver, const FrameSink &sink, Failure &failure) override
    {
        try {
            if (m_closed->load() || (m_window != nullptr && !IsWindow(m_window))) {
                failure.message = m_window != nullptr
                    ? QStringLiteral("The window being shared was closed.")
                    : QStringLiteral("The display being shared was disconnected.");
                return Pull::Failed;
            }
            BlackBox::Activity activity(area, "taking a frame from Windows.Graphics.Capture");
            // Keep only the newest: anything older is a picture already replaced.
            wgc::Direct3D11CaptureFrame newest{nullptr};
            for (;;) {
                wgc::Direct3D11CaptureFrame frame = m_pool.TryGetNextFrame();
                if (!frame)
                    break;
                if (newest)
                    newest.Close();
                newest = frame;
            }
            if (!newest)
                return redeliver && m_staging.isValid() ? deliver(sink) : Pull::Unchanged;

            const winrt::Windows::Graphics::SizeInt32 content = newest.ContentSize();
            {
                BlackBox::Activity copy(area, "copying a Windows.Graphics.Capture frame");
                ComPtr<ID3D11Texture2D> texture;
                DxgiInterfaceAccess *access = nullptr;
                winrt::check_hresult(static_cast<::IUnknown *>(winrt::get_abi(newest.Surface()))
                                         ->QueryInterface(iidDxgiInterfaceAccess,
                                                          reinterpret_cast<void **>(&access)));
                const HRESULT hr = access->GetInterface(
                    __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(texture.GetAddressOf()));
                access->Release();
                winrt::check_hresult(hr);
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                // The pool's buffers keep their size until it is recreated, so
                // a window that shrank leaves the rest of the buffer stale.
                const UINT width = std::min(UINT(std::max(content.Width, 1)), desc.Width);
                const UINT height = std::min(UINT(std::max(content.Height, 1)), desc.Height);
                if (!m_staging.ensure(m_device.Get(), width, height, desc.Format, false)) {
                    failure.message = QStringLiteral(
                        "OpenChat ran out of graphics memory while sharing the screen.");
                    return Pull::Failed;
                }
                D3D11_BOX box{0, 0, 0, width, height, 1};
                m_context->CopySubresourceRegion(m_staging.texture(), 0, 0, 0, 0, texture.Get(), 0,
                                                 &box);
                m_staging.markValid();
            }
            newest.Close();
            if (content.Width > 0 && content.Height > 0
                && (content.Width != m_size.Width || content.Height != m_size.Height)) {
                BlackBox::record(area, QStringLiteral("captured content resized to %1x%2")
                                           .arg(content.Width)
                                           .arg(content.Height));
                m_size = content;
                m_pool.Recreate(m_winrtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                                m_size);
            }
            return deliver(sink);
        } catch (const winrt::hresult_error &error) {
            failure.message = QStringLiteral("Windows stopped capturing %1 (%2).")
                                  .arg(m_subject, winrtErrorText(error));
            return Pull::Failed;
        }
    }

private:
    Pull deliver(const FrameSink &sink)
    {
        BlackBox::Activity activity(area, "reading the captured frame back from the GPU");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = m_context->Map(m_staging.texture(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr) || mapped.pData == nullptr)
            return Pull::Unchanged;
        const auto unmap = qScopeGuard([this] { m_context->Unmap(m_staging.texture(), 0); });
        D3D11_TEXTURE2D_DESC desc{};
        m_staging.texture()->GetDesc(&desc);
        ScreenFrameView view;
        view.bits = static_cast<const uchar *>(mapped.pData);
        view.bytesPerLine = qsizetype(mapped.RowPitch);
        view.width = int(desc.Width);
        view.height = int(desc.Height);
        // BGRA in memory is 0xAARRGGBB on a little-endian machine; the alpha of
        // a composed desktop means nothing, so it is read as opaque.
        view.format = QImage::Format_RGB32;
        sink(view);
        return Pull::Delivered;
    }

    void close()
    {
        try {
            m_closedRevoker.revoke();
            if (m_session)
                m_session.Close();
            if (m_pool)
                m_pool.Close();
        } catch (...) {
            // Closing is best effort; everything is released below regardless.
        }
        m_session = nullptr;
        m_pool = nullptr;
        m_item = nullptr;
        m_winrtDevice = nullptr;
        m_staging.reset();
        m_context.Reset();
        m_device.Reset();
    }

    HWND m_window = nullptr;
    HMONITOR m_monitor = nullptr;
    QString m_subject;
    bool m_software = false;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    wd3d::IDirect3DDevice m_winrtDevice{nullptr};
    wgc::GraphicsCaptureItem m_item{nullptr};
    wgc::Direct3D11CaptureFramePool m_pool{nullptr};
    wgc::GraphicsCaptureSession m_session{nullptr};
    wgc::GraphicsCaptureItem::Closed_revoker m_closedRevoker;
    winrt::Windows::Graphics::SizeInt32 m_size{0, 0};
    // Written from whatever thread WinRT raises Closed on.
    std::shared_ptr<std::atomic<bool>> m_closed = std::make_shared<std::atomic<bool>>(false);
    StagingFrame m_staging;
};

#else

[[nodiscard]] bool graphicsCaptureSupported()
{
    return false;
}

#endif // OPENCHAT_HAVE_WGC

[[nodiscard]] std::unique_ptr<NativeScreenCapture> makeGraphicsCapture(HWND window,
                                                                       HMONITOR monitor,
                                                                       const QString &subject)
{
#if OPENCHAT_HAVE_WGC
    return std::make_unique<GraphicsCapture>(window, monitor, subject);
#else
    Q_UNUSED(window);
    Q_UNUSED(monitor);
    Q_UNUSED(subject);
    return nullptr;
#endif
}

// --- GDI screen copy -----------------------------------------------------------

// The last resort, for a display neither Desktop Duplication nor
// Windows.Graphics.Capture will capture: a virtual machine with no GPU driver,
// some remote-desktop sessions, Wine. A plain BitBlt of the composed desktop
// into one reusable DIB section, and the pointer drawn on top the way the
// system draws it. Slower than either real API and it never knows what
// changed — the encoder's tile hashes find that out — but it works wherever
// there is a desktop at all.
class GdiScreenCapture final : public NativeScreenCapture
{
public:
    explicit GdiScreenCapture(QString deviceName)
        : m_deviceName(std::move(deviceName))
    {
    }

    ~GdiScreenCapture() override { release(); }

    QString describe() const override
    {
        return QStringLiteral("Windows GDI screen copy (fallback), %1, %2x%3")
            .arg(m_deviceName)
            .arg(m_rect.width())
            .arg(m_rect.height());
    }

    bool start(Failure &failure) override
    {
        BlackBox::Activity activity(area, "starting the GDI screen copy");
        if (!findMonitor()) {
            failure.message = QStringLiteral("The display being shared was disconnected.");
            return false;
        }
        m_screen = GetDC(nullptr);
        m_memory = m_screen != nullptr ? CreateCompatibleDC(m_screen) : nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = m_rect.width();
        // Negative: top-down rows, the order the encoder reads.
        info.bmiHeader.biHeight = -m_rect.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        m_bitmap = m_memory != nullptr
            ? CreateDIBSection(m_memory, &info, DIB_RGB_COLORS, reinterpret_cast<void **>(&m_bits),
                               nullptr, 0)
            : nullptr;
        if (m_bitmap == nullptr || m_bits == nullptr) {
            failure.message = QStringLiteral("OpenChat could not set up a copy of the screen "
                                             "(GDI error %1).")
                                  .arg(GetLastError());
            release();
            return false;
        }
        m_previous = SelectObject(m_memory, m_bitmap);
        return true;
    }

    Pull pull(bool redeliver, const FrameSink &sink, Failure &failure) override
    {
        // A whole-desktop BitBlt costs milliseconds of the main thread, so the
        // fallback copies at most ten times a second and hands the last copy
        // over again in between when the encoder needs a frame.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastCopyMs != 0 && now - m_lastCopyMs < gdiCopyIntervalMs)
            return redeliver ? deliver(sink) : Pull::Unchanged;
        m_lastCopyMs = now;
        BlackBox::Activity activity(area, "copying the screen with GDI");
        if (!findMonitor()) {
            failure.message = QStringLiteral("The display being shared was disconnected.");
            return Pull::Failed;
        }
        if (!BitBlt(m_memory, 0, 0, m_rect.width(), m_rect.height(), m_screen, m_rect.left(),
                    m_rect.top(), SRCCOPY)) {
            // The secure desktop (lock screen, UAC) refuses the copy; the
            // encoder simply gets nothing new until it comes back.
            return Pull::Unchanged;
        }
        CURSORINFO cursor{};
        cursor.cbSize = sizeof(cursor);
        if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) != 0) {
            ICONINFO icon{};
            if (GetIconInfo(cursor.hCursor, &icon)) {
                DrawIconEx(m_memory, cursor.ptScreenPos.x - m_rect.left() - int(icon.xHotspot),
                           cursor.ptScreenPos.y - m_rect.top() - int(icon.yHotspot), cursor.hCursor,
                           0, 0, 0, nullptr, DI_NORMAL);
                if (icon.hbmMask != nullptr)
                    DeleteObject(icon.hbmMask);
                if (icon.hbmColor != nullptr)
                    DeleteObject(icon.hbmColor);
            }
        }
        GdiFlush();
        return deliver(sink);
    }

private:
    Pull deliver(const FrameSink &sink)
    {
        ScreenFrameView view;
        view.bits = m_bits;
        view.bytesPerLine = qsizetype(m_rect.width()) * 4;
        view.width = m_rect.width();
        view.height = m_rect.height();
        view.format = QImage::Format_RGB32;
        sink(view);
        return Pull::Delivered;
    }

    // The monitor's rectangle in physical pixels, looked up by device name
    // each time so a resolution change or an unplugged monitor is noticed.
    bool findMonitor()
    {
        struct Search {
            const QString *name;
            QRect rect;
            bool found;
        } search{&m_deviceName, {}, false};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM parameter) -> BOOL {
                auto *state = reinterpret_cast<Search *>(parameter);
                MONITORINFOEXW info{};
                info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info) && QString::fromWCharArray(info.szDevice) == *state->name) {
                    const RECT &r = info.rcMonitor;
                    state->rect = QRect(QPoint(r.left, r.top), QSize(r.right - r.left, r.bottom - r.top));
                    state->found = true;
                    return FALSE;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&search));
        if (!search.found || search.rect.isEmpty())
            return false;
        // A changed resolution needs a new bitmap; ending the share is the
        // honest answer for a fallback, and the user can start it again.
        if (m_bits != nullptr && search.rect.size() != m_rect.size())
            return false;
        m_rect = search.rect;
        return true;
    }

    void release()
    {
        if (m_memory != nullptr && m_previous != nullptr)
            SelectObject(m_memory, m_previous);
        if (m_bitmap != nullptr)
            DeleteObject(m_bitmap);
        if (m_memory != nullptr)
            DeleteDC(m_memory);
        if (m_screen != nullptr)
            ReleaseDC(nullptr, m_screen);
        m_bitmap = nullptr;
        m_memory = nullptr;
        m_screen = nullptr;
        m_previous = nullptr;
        m_bits = nullptr;
    }

    QString m_deviceName;
    QRect m_rect;
    qint64 m_lastCopyMs = 0;
    HDC m_screen = nullptr;
    HDC m_memory = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_previous = nullptr;
    uchar *m_bits = nullptr;
};

// --- DXGI Desktop Duplication ------------------------------------------------

// The mouse pointer, which Desktop Duplication reports beside the desktop
// image rather than in it.
struct Pointer final {
    bool visible = false;
    POINT position{};
    bool shapeValid = false;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    std::vector<BYTE> shape;
};

// The pixels a pointer was drawn over, put back after the frame is handed on so
// the staging copy stays a clean desktop for the next redelivery.
struct Underneath final {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<quint32> pixels;
};

class DesktopDuplicationCapture final : public NativeScreenCapture
{
public:
    DesktopDuplicationCapture(QString deviceName, HMONITOR monitor)
        : m_deviceName(std::move(deviceName))
        , m_monitor(monitor)
    {
    }

    ~DesktopDuplicationCapture() override
    {
        releaseHeldFrame();
    }

    QString describe() const override
    {
        if (m_fallback)
            return m_fallback->describe() + QStringLiteral(" (Desktop Duplication refused this display)");
        QString text = QStringLiteral("Windows Desktop Duplication, ") + m_deviceName;
        if (m_width > 0)
            text += QStringLiteral(", %1x%2").arg(m_width).arg(m_height);
        if (m_rotationDegrees != 0)
            text += QStringLiteral(", rotated %1 degrees").arg(m_rotationDegrees);
        if (!m_adapterName.isEmpty())
            text += QStringLiteral(", on ") + m_adapterName;
        return text;
    }

    bool start(Failure &failure) override
    {
        HRESULT hr = S_OK;
        if (open(failure, hr))
            return true;
        // A moment's refusal — the lock screen, a UAC prompt, another
        // recorder, a disconnected session, a display that just went — is
        // reported as it is; trying again later is the answer to those.
        const bool momentary = hr == E_ACCESSDENIED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
            || hr == DXGI_ERROR_SESSION_DISCONNECTED || hr == DXGI_ERROR_NOT_FOUND;
        if (momentary)
            return false;
        // Anything else is Desktop Duplication refusing this setup outright:
        // a display it cannot reach from this GPU, an HDR format an older
        // Windows will not convert, a virtual machine or remote session
        // without it. Windows.Graphics.Capture takes the same monitor from
        // the compositor instead; GDI copies it when even that is missing.
        BlackBox::record(area, QStringLiteral("Desktop Duplication refused %1 (%2): %3")
                                   .arg(m_deviceName, hresultCode(hr), failure.message));
        QString attempts = failure.message;
        if (graphicsCaptureSupported()) {
            HMONITOR monitor = m_monitor;
            for (const DxgiOutput &output : enumerateOutputs()) {
                if (QString::fromWCharArray(output.desc.DeviceName) == m_deviceName)
                    monitor = output.desc.Monitor;
            }
            std::unique_ptr<NativeScreenCapture> capture =
                makeGraphicsCapture(nullptr, monitor, QStringLiteral("display ") + m_deviceName);
            Failure graphicsFailure;
            if (capture && capture->start(graphicsFailure)) {
                BlackBox::record(area, "fell back to Windows.Graphics.Capture");
                m_fallback = std::move(capture);
                return true;
            }
            attempts += QStringLiteral(" Windows.Graphics.Capture: ") + graphicsFailure.message;
        }
        auto gdi = std::make_unique<GdiScreenCapture>(m_deviceName);
        Failure gdiFailure;
        if (gdi->start(gdiFailure)) {
            BlackBox::record(area, "fell back to the GDI screen copy");
            m_fallback = std::move(gdi);
            return true;
        }
        failure.message = attempts + QStringLiteral(" GDI: ") + gdiFailure.message;
        return false;
    }

    Pull pull(bool redeliver, const FrameSink &sink, Failure &failure) override
    {
        if (m_fallback)
            return m_fallback->pull(redeliver, sink, failure);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!m_duplication) {
            // Lost: the secure desktop took over (lock screen, UAC prompt), the
            // display mode changed, or a full-screen program claimed the output.
            // Reopen it; meanwhile the last picture keeps going out, so a
            // moment's interruption does not end the share for everybody.
            if (now - m_lastReopenMs >= reopenIntervalMs) {
                m_lastReopenMs = now;
                Failure reopenFailure;
                HRESULT hr = S_OK;
                if (open(reopenFailure, hr)) {
                    BlackBox::record(area, QStringLiteral("Desktop Duplication recovered after %1 ms")
                                               .arg(now - m_lostSinceMs));
                    m_lostSinceMs = 0;
                } else if (hr == DXGI_ERROR_NOT_FOUND || now - m_lostSinceMs >= lostGiveUpMs) {
                    failure.message = hr == DXGI_ERROR_NOT_FOUND
                        ? reopenFailure.message
                        : QStringLiteral("Windows stopped the screen capture and did not allow "
                                         "it to resume: %1")
                              .arg(reopenFailure.message);
                    return Pull::Failed;
                }
            }
            if (!m_duplication)
                return redeliver && m_staging.isValid() ? deliver(sink) : Pull::Unchanged;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr;
        {
            BlackBox::Activity activity(area, "asking Desktop Duplication for a frame");
            hr = m_duplication->AcquireNextFrame(0, &info, resource.GetAddressOf());
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return redeliver && m_staging.isValid() ? deliver(sink) : Pull::Unchanged;
        if (FAILED(hr)) {
            BlackBox::record(area, QStringLiteral("Desktop Duplication lost (%1); reopening")
                                       .arg(describeHresult(hr)));
            m_duplication.Reset();
            m_lostSinceMs = now;
            m_lastReopenMs = 0;
            return redeliver && m_staging.isValid() ? deliver(sink) : Pull::Unchanged;
        }
        m_frameHeld = true;
        // Every successful acquire must be released before the next, whatever
        // happens below; deliver() releases it early, as soon as the copy has
        // landed, so the compositor is not held up by the encoder.
        const auto release = qScopeGuard([this] { releaseHeldFrame(); });

        bool changed = false;
        if (info.LastMouseUpdateTime.QuadPart != 0) {
            m_pointer.visible = info.PointerPosition.Visible != FALSE;
            m_pointer.position = info.PointerPosition.Position;
            changed = true;
        }
        if (info.PointerShapeBufferSize > 0) {
            BlackBox::Activity activity(area, "reading the mouse pointer's shape");
            m_pointer.shape.resize(info.PointerShapeBufferSize);
            UINT required = 0;
            m_pointer.shapeValid = SUCCEEDED(m_duplication->GetFramePointerShape(
                UINT(m_pointer.shape.size()), m_pointer.shape.data(), &required, &m_pointer.info));
            changed = true;
        }
        if (info.LastPresentTime.QuadPart != 0 && resource) {
            BlackBox::Activity activity(area, "copying the desktop image on the GPU");
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) {
                D3D11_TEXTURE2D_DESC desc{};
                texture->GetDesc(&desc);
                if (!m_staging.ensure(m_device.Get(), desc.Width, desc.Height, desc.Format, true)) {
                    failure.message = QStringLiteral(
                        "OpenChat ran out of graphics memory while sharing the screen.");
                    return Pull::Failed;
                }
                m_context->CopyResource(m_staging.texture(), texture.Get());
                m_staging.markValid();
                changed = true;
            }
        }
        if (!m_staging.isValid() || (!changed && !redeliver))
            return Pull::Unchanged;
        return deliver(sink);
    }

private:
    bool open(Failure &failure, HRESULT &hr)
    {
        BlackBox::Activity activity(area, "opening Desktop Duplication");
        m_formatUnsupported = false;
        std::vector<DxgiOutput> outputs = enumerateOutputs(&hr);
        const auto found = std::find_if(outputs.begin(), outputs.end(), [this](const DxgiOutput &o) {
            return QString::fromWCharArray(o.desc.DeviceName) == m_deviceName;
        });
        if (found == outputs.end()) {
            hr = DXGI_ERROR_NOT_FOUND;
            failure.message = QStringLiteral("The display being shared was disconnected.");
            return false;
        }

        // Built into locals and swapped in only on success, so a failed reopen
        // leaves the last good frame in place to keep redelivering.
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        // The device must live on the adapter that drives this output:
        // duplication is refused from any other.
        hr = createDevice(found->adapter.Get(), device, context);
        if (FAILED(hr)) {
            failure.message = QStringLiteral("OpenChat could not use the graphics card driving this "
                                             "display (%1).")
                                  .arg(describeHresult(hr));
            return false;
        }

        ComPtr<IDXGIOutputDuplication> duplication;
        // DuplicateOutput1 (Windows 10 1703) converts HDR and wide-colour
        // desktops to plain 8-bit BGRA; the original only duplicates whatever
        // format the desktop happens to be in.
        hr = E_NOINTERFACE;
        ComPtr<IDXGIOutput5> output5;
        if (SUCCEEDED(found->output.As(&output5))) {
            const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
            hr = output5->DuplicateOutput1(device.Get(), 0, UINT(std::size(formats)), formats,
                                           duplication.GetAddressOf());
        }
        if (FAILED(hr)) {
            ComPtr<IDXGIOutput1> output1;
            const HRESULT query = found->output.As(&output1);
            hr = FAILED(query) ? query
                               : output1->DuplicateOutput(device.Get(), duplication.GetAddressOf());
        }
        if (FAILED(hr)) {
            failure.message = duplicationFailureMessage(hr);
            return false;
        }
        DXGI_OUTDUPL_DESC desc{};
        duplication->GetDesc(&desc);
        if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            m_formatUnsupported = true;
            hr = DXGI_ERROR_UNSUPPORTED;
            failure.message = QStringLiteral(
                "This display is in a colour format screen sharing cannot read (HDR or 10-bit, "
                "DXGI format %1). Turn HDR off for this display in Windows' display settings, or "
                "share a window instead.")
                                  .arg(int(desc.ModeDesc.Format));
            return false;
        }

        releaseHeldFrame();
        m_device = std::move(device);
        m_context = std::move(context);
        m_duplication = std::move(duplication);
        m_staging.reset();
        m_adapterName = found->adapterName;
        m_width = int(desc.ModeDesc.Width);
        m_height = int(desc.ModeDesc.Height);
        switch (desc.Rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            m_rotationDegrees = 90;
            break;
        case DXGI_MODE_ROTATION_ROTATE180:
            m_rotationDegrees = 180;
            break;
        case DXGI_MODE_ROTATION_ROTATE270:
            m_rotationDegrees = 270;
            break;
        default:
            m_rotationDegrees = 0;
            break;
        }
        BlackBox::record(area, QStringLiteral("Desktop Duplication open: ") + describe());
        return true;
    }

    [[nodiscard]] static QString duplicationFailureMessage(HRESULT hr)
    {
        switch (hr) {
        case E_ACCESSDENIED:
            return QStringLiteral("Windows is not allowing screen capture right now: the lock "
                                  "screen or a security prompt may be showing (%1).")
                .arg(hresultCode(hr));
        case DXGI_ERROR_UNSUPPORTED:
            return QStringLiteral("Windows cannot duplicate this display (%1). This happens when "
                                  "OpenChat runs on a different graphics card from the one the "
                                  "display is connected to.")
                .arg(hresultCode(hr));
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
            return QStringLiteral("Too many programs are capturing this display at once. Close "
                                  "another screen recorder and try again (%1).")
                .arg(hresultCode(hr));
        case DXGI_ERROR_SESSION_DISCONNECTED:
            return QStringLiteral("The Windows session is disconnected, so nothing can be "
                                  "captured (%1).")
                .arg(hresultCode(hr));
        default:
            return QStringLiteral("Windows could not start capturing this display (%1).")
                .arg(describeHresult(hr));
        }
    }

    void releaseHeldFrame()
    {
        if (m_frameHeld && m_duplication)
            m_duplication->ReleaseFrame();
        m_frameHeld = false;
    }

    Pull deliver(const FrameSink &sink)
    {
        BlackBox::Activity activity(area, "reading the captured frame back from the GPU");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = m_context->Map(m_staging.texture(), 0, D3D11_MAP_READ_WRITE, 0, &mapped);
        // The copy has landed once Map returns, so the compositor can have its
        // surface back before the encoder starts.
        releaseHeldFrame();
        if (FAILED(hr) || mapped.pData == nullptr) {
            if (!m_mapFailureRecorded) {
                BlackBox::record(area, QStringLiteral("could not read the frame back: %1")
                                           .arg(describeHresult(hr)));
                m_mapFailureRecorded = true;
            }
            return Pull::Unchanged;
        }
        const auto unmap = qScopeGuard([this] { m_context->Unmap(m_staging.texture(), 0); });
        D3D11_TEXTURE2D_DESC desc{};
        m_staging.texture()->GetDesc(&desc);
        auto *bits = static_cast<uchar *>(mapped.pData);
        const int width = int(desc.Width);
        const int height = int(desc.Height);
        const qsizetype stride = qsizetype(mapped.RowPitch);

        if (m_rotationDegrees != 0) {
            // The duplicated image is in the panel's own orientation; a display
            // turned to portrait has to be turned back. Only rotated displays
            // pay for this copy.
            BlackBox::Activity rotate(area, "rotating the frame of a rotated display");
            const QImage panel(bits, width, height, stride, QImage::Format_RGB32);
            m_rotated = panel.transformed(QTransform().rotate(m_rotationDegrees));
            if (m_rotated.format() != QImage::Format_RGB32)
                m_rotated = m_rotated.convertToFormat(QImage::Format_RGB32);
            if (m_rotated.isNull())
                return Pull::Unchanged;
            drawPointer(m_rotated.bits(), m_rotated.width(), m_rotated.height(),
                        m_rotated.bytesPerLine(), nullptr);
            sink(ScreenFrameView::fromImage(m_rotated));
            return Pull::Delivered;
        }

        drawPointer(bits, width, height, stride, &m_underneath);
        ScreenFrameView view;
        view.bits = bits;
        view.bytesPerLine = stride;
        view.width = width;
        view.height = height;
        // BGRA in memory is 0xAARRGGBB on a little-endian machine, and a
        // duplicated desktop's alpha is undefined, so it is read as opaque.
        view.format = QImage::Format_RGB32;
        sink(view);
        restoreUnderneath(bits, stride);
        return Pull::Delivered;
    }

    // Draws the pointer the way the compositor would: straight alpha for a
    // colour pointer, AND/XOR for a monochrome one, XOR where a masked-colour
    // pointer's mask says so. Clipped to the frame; a shape whose buffer does
    // not cover what its header claims is skipped rather than read past.
    void drawPointer(uchar *bits, int width, int height, qsizetype stride, Underneath *save)
    {
        if (save != nullptr)
            save->width = 0;
        if (!m_pointer.visible || !m_pointer.shapeValid)
            return;
        BlackBox::Activity activity(area, "drawing the mouse pointer into the frame");
        const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info = m_pointer.info;
        const bool monochrome = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
        const int shapeWidth = int(info.Width);
        const int shapeHeight = monochrome ? int(info.Height / 2) : int(info.Height);
        const size_t pitch = info.Pitch;
        const size_t rowsNeeded = monochrome ? size_t(shapeHeight) * 2 : size_t(shapeHeight);
        if (shapeWidth <= 0 || shapeHeight <= 0 || m_pointer.shape.size() < pitch * rowsNeeded)
            return;
        if (!monochrome && pitch < size_t(shapeWidth) * 4)
            return;
        if (monochrome && pitch < size_t(shapeWidth + 7) / 8)
            return;

        const int left = m_pointer.position.x;
        const int top = m_pointer.position.y;
        const int x0 = std::max(0, left);
        const int y0 = std::max(0, top);
        const int x1 = std::min(width, left + shapeWidth);
        const int y1 = std::min(height, top + shapeHeight);
        if (x0 >= x1 || y0 >= y1)
            return;

        if (save != nullptr) {
            save->x = x0;
            save->y = y0;
            save->width = x1 - x0;
            save->height = y1 - y0;
            save->pixels.resize(size_t(save->width) * size_t(save->height));
            for (int y = y0; y < y1; ++y) {
                std::memcpy(save->pixels.data() + size_t(y - y0) * size_t(save->width),
                            bits + y * stride + qsizetype(x0) * 4, size_t(save->width) * 4);
            }
        }

        const BYTE *shape = m_pointer.shape.data();
        for (int y = y0; y < y1; ++y) {
            auto *row = reinterpret_cast<quint32 *>(bits + y * stride);
            const int sy = y - top;
            for (int x = x0; x < x1; ++x) {
                const int sx = x - left;
                quint32 &pixel = row[x];
                if (monochrome) {
                    const BYTE bit = BYTE(0x80 >> (sx % 8));
                    const bool andBit = (shape[size_t(sy) * pitch + size_t(sx / 8)] & bit) != 0;
                    const bool xorBit =
                        (shape[size_t(sy + shapeHeight) * pitch + size_t(sx / 8)] & bit) != 0;
                    pixel = ((pixel & (andBit ? 0xFFFFFFFFu : 0xFF000000u))
                             ^ (xorBit ? 0x00FFFFFFu : 0u))
                        | 0xFF000000u;
                    continue;
                }
                quint32 source;
                std::memcpy(&source, shape + size_t(sy) * pitch + size_t(sx) * 4, 4);
                if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                    pixel = (source & 0xFF000000u) != 0 ? ((pixel ^ source) | 0xFF000000u)
                                                        : (source | 0xFF000000u);
                    continue;
                }
                const quint32 alpha = source >> 24;
                if (alpha == 0)
                    continue;
                if (alpha == 255) {
                    pixel = source | 0xFF000000u;
                    continue;
                }
                quint32 blended = 0xFF000000u;
                for (int shift = 0; shift <= 16; shift += 8) {
                    const quint32 s = (source >> shift) & 0xFFu;
                    const quint32 d = (pixel >> shift) & 0xFFu;
                    blended |= ((s * alpha + d * (255 - alpha)) / 255) << shift;
                }
                pixel = blended;
            }
        }
    }

    void restoreUnderneath(uchar *bits, qsizetype stride)
    {
        if (m_underneath.width <= 0)
            return;
        for (int row = 0; row < m_underneath.height; ++row) {
            std::memcpy(bits + (m_underneath.y + row) * stride + qsizetype(m_underneath.x) * 4,
                        m_underneath.pixels.data() + size_t(row) * size_t(m_underneath.width),
                        size_t(m_underneath.width) * 4);
        }
        m_underneath.width = 0;
    }

    QString m_deviceName;
    HMONITOR m_monitor = nullptr;
    QString m_adapterName;
    int m_width = 0;
    int m_height = 0;
    int m_rotationDegrees = 0;
    bool m_formatUnsupported = false;
    bool m_frameHeld = false;
    bool m_mapFailureRecorded = false;
    qint64 m_lostSinceMs = 0;
    qint64 m_lastReopenMs = 0;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    StagingFrame m_staging;
    Pointer m_pointer;
    Underneath m_underneath;
    QImage m_rotated;
    // Set when Desktop Duplication refused the display and
    // Windows.Graphics.Capture took over.
    std::unique_ptr<NativeScreenCapture> m_fallback;
};

} // namespace

namespace NativeScreenCapturePlatform {

bool isAvailable()
{
    return qEnvironmentVariable("OPENCHAT_SCREEN_CAPTURE").trimmed().toLower()
        != QStringLiteral("qt");
}

QString name()
{
    if (!isAvailable())
        return {};
    const QString screens = preferredScreenApi() == ScreenApi::GraphicsCapture
        ? QStringLiteral("Windows.Graphics.Capture")
        : preferredScreenApi() == ScreenApi::Gdi ? QStringLiteral("Windows GDI screen copy")
                                                 : QStringLiteral("Windows Desktop Duplication");
    return graphicsCaptureSupported() && preferredScreenApi() != ScreenApi::GraphicsCapture
        ? screens + QStringLiteral(" and Windows.Graphics.Capture")
        : screens;
}

QVector<ScreenShareSource> sources()
{
    QVector<ScreenShareSource> result;
    HRESULT error = S_OK;
    const std::vector<DxgiOutput> outputs = enumerateOutputs(&error);
    if (FAILED(error))
        BlackBox::record(area, QStringLiteral("could not list displays: ") + describeHresult(error));
    int ordinal = 0;
    for (const DxgiOutput &output : outputs) {
        ++ordinal;
        ScreenShareSource source;
        source.kind = ScreenShareSource::Kind::Screen;
        source.native = true;
        source.nativeName = QString::fromWCharArray(output.desc.DeviceName);
        source.nativeId = quint64(reinterpret_cast<quintptr>(output.desc.Monitor));
        const QRect rect = desktopRect(output.desc);
        source.size = rect.size();
        source.screen = matchQtScreen(source.nativeName, rect);
        source.id = QStringLiteral("screen:") + source.nativeName;
        source.name = outputs.size() > 1
            ? QStringLiteral("Display %1%2 (%3×%4)")
                  .arg(displayNumber(source.nativeName, ordinal))
                  .arg(output.primary ? QStringLiteral(" — main") : QString())
                  .arg(rect.width())
                  .arg(rect.height())
            : QStringLiteral("Entire screen (%1×%2)").arg(rect.width()).arg(rect.height());
        result.append(source);
    }
    if (!graphicsCaptureSupported())
        return result;
    std::vector<WindowCandidate> windows;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&windows));
    for (const WindowCandidate &window : windows) {
        ScreenShareSource source;
        source.kind = ScreenShareSource::Kind::Window;
        source.native = true;
        source.nativeId = quint64(reinterpret_cast<quintptr>(window.handle));
        source.name = window.title;
        source.size = window.size;
        source.id = QStringLiteral("window:") + QString::number(source.nativeId);
        result.append(source);
    }
    return result;
}

std::unique_ptr<NativeScreenCapture> create(const ScreenShareSource &source)
{
    if (!source.native)
        return nullptr;
    if (source.kind == ScreenShareSource::Kind::Window) {
        auto *window = reinterpret_cast<HWND>(quintptr(source.nativeId));
        const QString process = processNameOf(window);
        return makeGraphicsCapture(window, nullptr,
                                   process.isEmpty() ? QStringLiteral("a window")
                                                     : QStringLiteral("a window of ") + process);
    }
    auto *monitor = reinterpret_cast<HMONITOR>(quintptr(source.nativeId));
    if (preferredScreenApi() == ScreenApi::GraphicsCapture && graphicsCaptureSupported())
        return makeGraphicsCapture(nullptr, monitor, QStringLiteral("display ") + source.nativeName);
    if (preferredScreenApi() == ScreenApi::Gdi)
        return std::make_unique<GdiScreenCapture>(source.nativeName);
    return std::make_unique<DesktopDuplicationCapture>(source.nativeName, monitor);
}

ScreenCaptureAccess access(bool)
{
    // Unpackaged desktop applications need no permission to capture.
    return ScreenCaptureAccess::Granted;
}

QString accessDeniedMessage()
{
    return {};
}

bool openAccessSettings()
{
    return false;
}

} // namespace NativeScreenCapturePlatform

} // namespace OpenChat
