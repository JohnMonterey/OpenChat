#include "notify/WindowsNotifier.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

// clang-format off
#include <windows.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
// clang-format on

// C++/WinRT ships with the Windows SDK and with recent mingw-w64, so both give
// real toasts; a toolchain without it falls back to the notification-area
// balloon below. Define OPENCHAT_HAVE_WINRT_TOASTS to 0 to force the fallback
// (the build defines it to match, and it is how both paths are compiled when
// checking this file).
#if !defined(OPENCHAT_HAVE_WINRT_TOASTS)
#    if __has_include(<winrt/Windows.UI.Notifications.h>)
#        define OPENCHAT_HAVE_WINRT_TOASTS 1
#    else
#        define OPENCHAT_HAVE_WINRT_TOASTS 0
#    endif
#endif

#if OPENCHAT_HAVE_WINRT_TOASTS
#    include <winrt/Windows.Data.Xml.Dom.h>
#    include <winrt/Windows.Foundation.h>
#    include <winrt/Windows.UI.Notifications.h>
#endif

namespace OpenChat {

namespace {

// Toast tags are limited to 64 characters, so a conversation key is reduced to
// a stable hex digest that is also safe inside XML and the shell's stores.
QString tagFor(const QString &key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

std::wstring toWide(const QString &text)
{
    return std::wstring(reinterpret_cast<const wchar_t *>(text.utf16()),
                        static_cast<size_t>(text.size()));
}

// Copies into one of the shell's fixed-size wide buffers, always terminated.
// Written by hand because the bounds-checked C library functions are not
// portable across the toolchains this file is built with.
template <size_t N> void copyFixed(wchar_t (&destination)[N], const std::wstring &text)
{
    const size_t count = std::min(text.size(), N - 1);
    if (count > 0)
        std::memcpy(destination, text.data(), count * sizeof(wchar_t));
    destination[count] = L'\0';
}

// XML text nodes carry the sender's name and message, both of which are
// arbitrary user text.
[[maybe_unused]] QString escapeXml(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    escaped.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    escaped.replace(QLatin1Char('>'), QLatin1String("&gt;"));
    escaped.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    escaped.replace(QLatin1Char('\''), QLatin1String("&apos;"));
    return escaped;
}

// Windows attributes a toast to an Application User Model ID, and an
// unpackaged application only owns one if a Start Menu shortcut declares it.
// Creating the shortcut once is what makes toasts appear at all.
bool ensureStartMenuShortcut(const QString &appName, const QString &appUserModelId)
{
    const QString startMenu =
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (startMenu.isEmpty())
        return false;
    const QString linkPath = QDir(startMenu).filePath(appName + QStringLiteral(".lnk"));
    if (QFile::exists(linkPath))
        return true;
    QDir().mkpath(startMenu);

    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))))
        return false;

    bool created = false;
    const std::wstring exePath = toWide(QDir::toNativeSeparators(
        QCoreApplication::applicationFilePath()));
    const std::wstring workingDir =
        toWide(QDir::toNativeSeparators(QCoreApplication::applicationDirPath()));
    if (SUCCEEDED(link->SetPath(exePath.c_str()))
        && SUCCEEDED(link->SetWorkingDirectory(workingDir.c_str()))) {
        IPropertyStore *properties = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&properties)))) {
            PROPVARIANT value;
            const std::wstring aumid = toWide(appUserModelId);
            if (SUCCEEDED(InitPropVariantFromString(aumid.c_str(), &value))) {
                if (SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ID, value))
                    && SUCCEEDED(properties->Commit())) {
                    IPersistFile *persist = nullptr;
                    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))) {
                        created = SUCCEEDED(persist->Save(toWide(
                            QDir::toNativeSeparators(linkPath)).c_str(), TRUE));
                        persist->Release();
                    }
                }
                PropVariantClear(&value);
            }
            properties->Release();
        }
    }
    link->Release();
    return created;
}

#if !OPENCHAT_HAVE_WINRT_TOASTS

// Turns an image into an icon for the balloon. Qt 6 dropped the Windows extras
// that used to do this, so the two bitmaps an icon needs are built by hand.
HICON toHIcon(const QImage &image)
{
    if (image.isNull())
        return nullptr;
    const QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    BITMAPV5HEADER header = {};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = source.width();
    header.bV5Height = -source.height(); // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;

    HDC screen = GetDC(nullptr);
    void *bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&header),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr || bits == nullptr)
        return nullptr;

    for (int y = 0; y < source.height(); ++y) {
        std::memcpy(static_cast<uchar *>(bits) + static_cast<size_t>(y) * source.width() * 4,
                    source.constScanLine(y), static_cast<size_t>(source.width()) * 4);
    }

    // A 32-bit icon still needs a mask bitmap, even though alpha does the work.
    HBITMAP mask = CreateBitmap(source.width(), source.height(), 1, 1, nullptr);
    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

constexpr UINT trayCallbackMessage = WM_APP + 71;
const wchar_t trayWindowClass[] = L"OpenChatNotificationSink";

#endif // !OPENCHAT_HAVE_WINRT_TOASTS

} // namespace

struct WindowsNotifier::Private {
    NotificationAppInfo appInfo;
    bool registered = false;
    QString imageDir;
    // The image files handed to Windows, kept until their toast is replaced or
    // withdrawn so the shell can still read them.
    QHash<QString, QString> imageByKey;

#if !OPENCHAT_HAVE_WINRT_TOASTS
    HWND window = nullptr;
    NOTIFYICONDATAW iconData = {};
    bool iconAdded = false;
    HICON balloonIcon = nullptr;
    // The balloon on screen: the shell shows one at a time, so only the most
    // recent conversation can be activated by clicking it.
    QString currentKey;
#endif
};

#if !OPENCHAT_HAVE_WINRT_TOASTS
namespace {

LRESULT CALLBACK trayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == trayCallbackMessage && LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
        auto *backend = reinterpret_cast<WindowsNotifier *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (backend != nullptr)
            backend->reportActivated(QString());
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace
#endif

WindowsNotifier::WindowsNotifier(NotificationAppInfo appInfo, QObject *parent)
    : NotificationBackend(parent), d(std::make_unique<Private>())
{
    d->appInfo = std::move(appInfo);

    // Qt may already have initialised COM on this thread; either outcome is
    // fine, and a mismatched mode simply means someone else owns it.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Q_UNUSED(comResult);

    const std::wstring aumid = toWide(d->appInfo.appUserModelId);
    SetCurrentProcessExplicitAppUserModelID(aumid.c_str());
    ensureStartMenuShortcut(d->appInfo.applicationName, d->appInfo.appUserModelId);

    d->imageDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                      .filePath(QStringLiteral("OpenChat-notifications"));
    QDir().mkpath(d->imageDir);

#if OPENCHAT_HAVE_WINRT_TOASTS
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
        // Already initialised in another mode: WinRT calls still work.
    }
    d->registered = true;
#else
    // A message-only window receives the shell's balloon callbacks.
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = trayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = trayWindowClass;
    RegisterClassExW(&windowClass); // harmless if already registered
    d->window = CreateWindowExW(0, trayWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                windowClass.hInstance, nullptr);
    if (d->window != nullptr) {
        SetWindowLongPtrW(d->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        d->iconData.cbSize = sizeof(NOTIFYICONDATAW);
        d->iconData.hWnd = d->window;
        d->iconData.uID = 1;
        d->iconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        d->iconData.uCallbackMessage = trayCallbackMessage;
        d->iconData.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        copyFixed(d->iconData.szTip, toWide(d->appInfo.applicationName));
        d->iconAdded = Shell_NotifyIconW(NIM_ADD, &d->iconData) == TRUE;
        d->registered = d->iconAdded;
    }
#endif
}

WindowsNotifier::~WindowsNotifier()
{
    withdrawAll();
#if !OPENCHAT_HAVE_WINRT_TOASTS
    if (d->iconAdded)
        Shell_NotifyIconW(NIM_DELETE, &d->iconData);
    if (d->balloonIcon != nullptr)
        DestroyIcon(d->balloonIcon);
    if (d->window != nullptr)
        DestroyWindow(d->window);
#endif
    if (!d->imageDir.isEmpty())
        QDir(d->imageDir).removeRecursively();
}

bool WindowsNotifier::isAvailable() const
{
    return d->registered;
}

QString WindowsNotifier::name() const
{
#if OPENCHAT_HAVE_WINRT_TOASTS
    return QStringLiteral("windows-toast");
#else
    return QStringLiteral("windows-tray");
#endif
}

void WindowsNotifier::show(const Notification &notification)
{
    if (!isAvailable())
        return;

    // The picture is handed to the shell as a file. Each post writes its own so
    // that replacing a notification never rewrites a file Windows is reading.
    QString imagePath;
    if (!notification.image.isNull() && !d->imageDir.isEmpty()) {
        const QString candidate =
            QDir(d->imageDir)
                .filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)
                          + QStringLiteral(".png"));
        if (notification.image.save(candidate, "PNG"))
            imagePath = candidate;
    }
    // The file backing the notification this one replaces is no longer needed.
    const QString previousImage = d->imageByKey.value(notification.key);
    if (!previousImage.isEmpty() && previousImage != imagePath)
        QFile::remove(previousImage);
    if (imagePath.isEmpty())
        d->imageByKey.remove(notification.key);
    else
        d->imageByKey.insert(notification.key, imagePath);

#if OPENCHAT_HAVE_WINRT_TOASTS
    namespace WinNotifications = winrt::Windows::UI::Notifications;
    namespace WinXml = winrt::Windows::Data::Xml::Dom;

    QString imageElement;
    if (!imagePath.isEmpty()) {
        imageElement =
            QStringLiteral("<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"%1\"/>")
                .arg(escapeXml(QUrl::fromLocalFile(imagePath).toString()));
    }
    const QString xml =
        QStringLiteral("<toast launch=\"%1\"><visual><binding template=\"ToastGeneric\">"
                       "<text>%2</text><text>%3</text>%4"
                       "</binding></visual></toast>")
            .arg(escapeXml(notification.key), escapeXml(notification.title),
                 escapeXml(notification.body), imageElement);

    try {
        WinXml::XmlDocument document;
        document.LoadXml(winrt::hstring(toWide(xml)));
        WinNotifications::ToastNotification toast(document);
        const std::wstring tag = toWide(tagFor(notification.key));
        toast.Tag(winrt::hstring(tag));
        toast.Group(winrt::hstring(toWide(d->appInfo.applicationName)));

        QPointer<WindowsNotifier> guard(this);
        const QString key = notification.key;
        toast.Activated([guard, key](const WinNotifications::ToastNotification &,
                                     const winrt::Windows::Foundation::IInspectable &) {
            if (qApp == nullptr)
                return;
            // The event arrives on a WinRT thread; hop to the Qt event loop.
            QMetaObject::invokeMethod(
                qApp,
                [guard, key] {
                    if (guard)
                        guard->reportActivated(key);
                },
                Qt::QueuedConnection);
        });

        WinNotifications::ToastNotificationManager::CreateToastNotifier(
            winrt::hstring(toWide(d->appInfo.appUserModelId)))
            .Show(toast);
    } catch (...) {
        // A desktop with notifications turned off for this application throws
        // rather than failing quietly; there is nothing useful to do about it.
    }
#else
    // Balloon fallback: the picture becomes the balloon's icon.
    HICON icon = toHIcon(notification.image);
    d->iconData.uFlags = NIF_INFO | NIF_MESSAGE | NIF_ICON | NIF_TIP;
    d->iconData.dwInfoFlags = icon != nullptr ? NIIF_USER | NIIF_LARGE_ICON : NIIF_INFO;
    d->iconData.hBalloonIcon = icon;
    copyFixed(d->iconData.szInfoTitle, toWide(notification.title));
    copyFixed(d->iconData.szInfo, toWide(notification.body));
    Shell_NotifyIconW(NIM_MODIFY, &d->iconData);
    if (d->balloonIcon != nullptr)
        DestroyIcon(d->balloonIcon);
    d->balloonIcon = icon;
    d->currentKey = notification.key;
#endif
}

void WindowsNotifier::withdraw(const QString &key)
{
    const QString image = d->imageByKey.take(key);
    if (!image.isEmpty())
        QFile::remove(image);
    if (!isAvailable())
        return;

#if OPENCHAT_HAVE_WINRT_TOASTS
    try {
        winrt::Windows::UI::Notifications::ToastNotificationManager::History().Remove(
            winrt::hstring(toWide(tagFor(key))),
            winrt::hstring(toWide(d->appInfo.applicationName)),
            winrt::hstring(toWide(d->appInfo.appUserModelId)));
    } catch (...) {
        // Nothing to take back.
    }
#else
    // The shell owns the balloon's lifetime; the most that can be done is to
    // stop attributing a later click to a conversation the user has now read.
    if (d->currentKey == key)
        d->currentKey.clear();
#endif
}

void WindowsNotifier::withdrawAll()
{
    const QList<QString> keys = d->imageByKey.keys();
    for (const QString &key : keys)
        withdraw(key);
#if OPENCHAT_HAVE_WINRT_TOASTS
    if (isAvailable()) {
        try {
            winrt::Windows::UI::Notifications::ToastNotificationManager::History().Clear(
                winrt::hstring(toWide(d->appInfo.appUserModelId)));
        } catch (...) {
        }
    }
#else
    d->currentKey.clear();
#endif
}

void WindowsNotifier::reportActivated(const QString &key)
{
#if OPENCHAT_HAVE_WINRT_TOASTS
    const QString target = key;
#else
    // The balloon callback carries no identity, so it always means the balloon
    // currently on screen.
    const QString target = key.isEmpty() ? d->currentKey : key;
    d->currentKey.clear();
#endif
    if (target.isEmpty())
        return;
    const QString image = d->imageByKey.take(target);
    if (!image.isEmpty())
        QFile::remove(image);
    emit activated(target);
}

} // namespace OpenChat
