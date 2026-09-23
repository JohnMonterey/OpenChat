#include <atomic>
#include <cmath>
#include <cstdlib>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHostInfo>
#include <QIcon>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQuickView>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <qqml.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>

#include "app/AccountBootstrap.h"
#include "app/CloseToTray.h"
#include "app/TrayIcon.h"
#include "app/LocalDataReset.h"
#include "diagnostics/Logging.h"
#include "app/AppMetadata.h"
#include "app/ContactRequestService.h"
#include "app/DeviceLink.h"
#include "app/GroupService.h"
#include "app/ProfileSession.h"
#include "call/CallEngine.h"
#include "call/NativeScreenCapture.h"
#include "call/ScreenVideoCodec.h"
#include "call/ScreenAudioCapture.h"
#include "call/QtAudioIo.h"
#include "call/SyncCallTransport.h"
#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/CrashReportController.h"
#include "diagnostics/BlackBox.h"
#include "diagnostics/CrashReporter.h"
#include "controllers/OnboardingController.h"
#include "controllers/VoiceDebugController.h"
#include "domain/Identifiers.h"
#include "network/RelayClient.h"
#include "notify/NotificationBackend.h"
#include "notify/NotificationService.h"
#include "network/RelayTransport.h"
#include "network/SyncEngine.h"
#include "render/AvatarArtwork.h"
#include "app/AppearanceSettings.h"
#include "app/MemorySettings.h"
#include "app/MicrophoneSettings.h"
#include "app/VoiceEffectHost.h"
#include "app/ComposerEditing.h"
#include "app/TextLineSpacing.h"
#include "call/ScreenCanvas.h"
#include "case/DailyCaseController.h"
#include "case/RelayCaseService.h"
#include "cosmetics/CosmeticTypes.h"
#include "app/TransportSettings.h"
#include "call/UdpCallMediaPath.h"
#include "render/CallVideoItem.h"
#include "render/BubbleBackground.h"
#include "security/KeyVault.h"
#include "security/PasswordKey.h"
#include "security/QtKeychainVault.h"
#include "security/RecoveryCode.h"

// Last, so its macros never reach the headers above.
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
// OpenChat is a GUI program on Windows, so starting it never opens a console.
// Run from a Command Prompt, though, what it prints (--help, the screen-share
// check, Qt's warnings) belongs in that prompt, so it borrows the console of
// whatever started it, if there is one. Output the caller redirected to a
// file or a pipe is already in place and stays where it was sent.
//
// Qt decides between the console and the debugger on the first message it
// logs, so this runs before anything else.
void attachParentConsole()
{
    const auto usable = [](DWORD which) {
        const HANDLE handle = GetStdHandle(which);
        return handle != nullptr && handle != INVALID_HANDLE_VALUE
               && GetFileType(handle) != FILE_TYPE_UNKNOWN;
    };
    const bool haveOut = usable(STD_OUTPUT_HANDLE);
    const bool haveErr = usable(STD_ERROR_HANDLE);
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return; // started from Explorer: there is no one to print to
    if (!haveOut)
        (void)std::freopen("CONOUT$", "w", stdout);
    if (!haveErr)
        (void)std::freopen("CONOUT$", "w", stderr);
}
#endif

// Registers the C++ types the QML surfaces consume. Registration is global to the
// process, so every engine and view created below resolves the same types.
void registerQmlTypes()
{
    qmlRegisterType<OpenChat::DailyCaseController>("OpenChat.Native", 1, 0, "DailyCaseController");
    qmlRegisterSingletonType<OpenChat::AppearanceSettings>(
        "OpenChat.Native", 1, 0, "AppearanceSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new OpenChat::AppearanceSettings; });
    // The microphone settings are shared with the call engine, so the QML side
    // is handed the process instance when main() has made one; the engine
    // never owns it. Only a bare QML load (the tests) gets its own.
    qmlRegisterSingletonType<OpenChat::MicrophoneSettings>(
        "OpenChat.Native", 1, 0, "MicrophoneSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            if (auto *shared = OpenChat::MicrophoneSettings::instance()) {
                QQmlEngine::setObjectOwnership(shared, QQmlEngine::CppOwnership);
                return shared;
            }
            return new OpenChat::MicrophoneSettings;
        });
    // The plugin host, on the same terms as the microphone settings: shared
    // with the call engine when main() has made one, and its own object only
    // for a bare QML load.
    qmlRegisterSingletonType<OpenChat::VoiceEffectHost>(
        "OpenChat.Native", 1, 0, "VoiceEffectHost",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            if (auto *shared = OpenChat::VoiceEffectHost::instance()) {
                QQmlEngine::setObjectOwnership(shared, QQmlEngine::CppOwnership);
                return shared;
            }
            return new OpenChat::VoiceEffectHost;
        });
    // Low memory mode, shared the same way: the application's instance is the
    // one that trims the heap and tells the avatar store what to keep.
    qmlRegisterSingletonType<OpenChat::MemorySettings>(
        "OpenChat.Native", 1, 0, "MemorySettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            if (auto *shared = OpenChat::MemorySettings::instance()) {
                QQmlEngine::setObjectOwnership(shared, QQmlEngine::CppOwnership);
                return shared;
            }
            return new OpenChat::MemorySettings;
        });
    // The UDP media preference, shared the same way.
    qmlRegisterSingletonType<OpenChat::TransportSettings>(
        "OpenChat.Native", 1, 0, "TransportSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            if (auto *shared = OpenChat::TransportSettings::instance()) {
                QQmlEngine::setObjectOwnership(shared, QQmlEngine::CppOwnership);
                return shared;
            }
            return new OpenChat::TransportSettings;
        });
    qmlRegisterType<OpenChat::BubbleBackground>("OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::CallVideoItem>("OpenChat.Native", 1, 0, "CallVideoItem");
    qmlRegisterType<OpenChat::AvatarArtwork>("OpenChat.Native", 1, 0, "AvatarArtwork");
    qmlRegisterType<OpenChat::ComposerEditing>("OpenChat.Native", 1, 0, "ComposerEditing");
    qmlRegisterType<OpenChat::TextLineSpacing>("OpenChat.Native", 1, 0, "TextLineSpacing");
    // Avatar frames, presence beads, name flair and profile scenes.
    OpenChat::registerCosmeticQmlTypes();
    qmlRegisterUncreatableType<OpenChat::ChatController>(
        "OpenChat.Native", 1, 0, "ChatController",
        QStringLiteral("ChatController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::OnboardingController>(
        "OpenChat.Native", 1, 0, "OnboardingController",
        QStringLiteral("OnboardingController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::ContactController>(
        "OpenChat.Native", 1, 0, "ContactController",
        QStringLiteral("ContactController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::CallController>(
        "OpenChat.Native", 1, 0, "CallController",
        QStringLiteral("CallController is provided by the application"));
    qmlRegisterUncreatableType<OpenChat::CallParticipantModel>(
        "OpenChat.Native", 1, 0, "CallParticipantModel",
        QStringLiteral("CallParticipantModel is provided by the CallController"));
    qmlRegisterUncreatableType<OpenChat::VoiceDebugController>(
        "OpenChat.Native", 1, 0, "VoiceDebugController",
        QStringLiteral("VoiceDebugController is provided by the application"));
}

// Applies an optional --width/--height override to a window, honouring the app's
// minimum bounds. Identical logic to the historical inline capture setup so the
// rendered chat window (and its capture) is unaffected.
void applyWindowSizing(QCommandLineParser &parser, QQuickWindow *window,
                       const QCommandLineOption &widthOption,
                       const QCommandLineOption &heightOption)
{
    bool widthValid = false;
    bool heightValid = false;
    const int requestedWidth = parser.value(widthOption).toInt(&widthValid);
    const int requestedHeight = parser.value(heightOption).toInt(&heightValid);
    if (widthValid && requestedWidth >= OpenChat::AppMetadata::minimumWidth)
        window->setWidth(requestedWidth);
    if (heightValid && requestedHeight >= OpenChat::AppMetadata::minimumHeight)
        window->setHeight(requestedHeight);
}

// Schedules a one-shot window grab when --capture is set, exiting the process
// with the grab's success. Byte-for-byte the historical capture behaviour.
void scheduleCaptureIfRequested(QCommandLineParser &parser, QQuickWindow *window,
                                const QCommandLineOption &captureOption,
                                const QCommandLineOption &delayOption)
{
    if (!parser.isSet(captureOption))
        return;
    bool delayValid = false;
    const int requestedDelay = parser.value(delayOption).toInt(&delayValid);
    const int delay = delayValid ? std::max(0, requestedDelay) : 500;
    const QString capturePath = QDir::current().absoluteFilePath(parser.value(captureOption));
    QTimer::singleShot(delay, window, [window, capturePath] {
        const bool saved = window->grabWindow().save(capturePath, "PNG");
        QCoreApplication::exit(saved ? EXIT_SUCCESS : EXIT_FAILURE);
    });
}

// When a dev-CA path is configured, returns a TLS configuration that ADDS that CA
// on top of the system trust roots while keeping full peer verification. Returns
// nullopt when no dev CA is configured (the client then uses system trust only).
// Verification is never disabled here or anywhere downstream.
std::optional<QSslConfiguration> buildDevCaTls(const QString &devCaPath)
{
    if (devCaPath.isEmpty())
        return std::nullopt;
    const QList<QSslCertificate> cas = QSslCertificate::fromPath(devCaPath);
    if (cas.isEmpty()) {
        qWarning().noquote() << QStringLiteral(
            "OpenChat: OPENCHAT_DEV_CA is set but no certificate loaded from it; "
            "using system trust only.");
        return std::nullopt;
    }
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    QList<QSslCertificate> roots = config.caCertificates();
    roots.append(cas);
    config.setCaCertificates(roots);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    config.setProtocol(QSsl::SecureProtocols);
    return config;
}

// Finds a single already-created local profile under `profilesRoot`. Profile
// directories are named by the profile id's lowercase hex; a directory qualifies
// only when the name round-trips to a valid ProfileId, holds a profile.sqlite3
// and is stamped with the current account layout (see app/LocalDataReset.h), so
// a profile left over from before usernames and passwords is never opened.
std::optional<OpenChat::ProfileId> findExistingProfile(const QString &profilesRoot)
{
    QDir root(profilesRoot);
    if (!root.exists())
        return std::nullopt;
    const QStringList entries =
        root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        const std::optional<OpenChat::ProfileId> id =
            OpenChat::ProfileId::fromBytes(QByteArray::fromHex(entry.toLatin1()));
        if (!id || id->toHex() != entry)
            continue;
        const OpenChat::ProfilePaths paths =
            OpenChat::ProfilePaths::forProfile(profilesRoot, *id);
        if (QFileInfo(paths.database).isFile() && OpenChat::profileHasCurrentAccountLayout(paths))
            return id;
    }
    return std::nullopt;
}

// Once the interface has been built, hands back what building it freed: the
// heap otherwise holds on to it for reuse that an idle chat window never
// makes, around 1.5 MB of the settled process.
void releaseFreedHeapLater(QObject *context)
{
    QTimer::singleShot(std::chrono::seconds(5), context,
                       &OpenChat::MemorySettings::releaseFreedHeap);
}

// Overwrites a byte array's contents before releasing it (key material that had
// to be copied out of a SecureBuffer to be sent).
void scrubBytes(QByteArray &bytes)
{
    volatile char *data = bytes.data();
    for (qsizetype i = 0; i < bytes.size(); ++i)
        data[i] = '\0';
    bytes.clear();
}

QString messageForBootstrapError(OpenChat::AccountBootstrap::Error error)
{
    switch (error) {
    case OpenChat::AccountBootstrap::Error::HandleUnavailable:
        return QStringLiteral(
            "That username is taken. Choose another, or log in if it's yours.");
    case OpenChat::AccountBootstrap::Error::InvalidCredentials:
        return QStringLiteral("Wrong username or password.");
    case OpenChat::AccountBootstrap::Error::RateLimited:
        return QStringLiteral(
            "Too many failed attempts for that username. Wait 15 minutes and try again.");
    case OpenChat::AccountBootstrap::Error::Auth:
        return QStringLiteral("Couldn't verify this device. Please try again.");
    case OpenChat::AccountBootstrap::Error::Publish:
    case OpenChat::AccountBootstrap::Error::Storage:
        return QStringLiteral("Couldn't finish setting up your account. Please try again.");
    case OpenChat::AccountBootstrap::Error::Transport:
        return QStringLiteral("Couldn't reach OpenChat. Check your connection and try again.");
    }
    return QStringLiteral("Something went wrong. Please try again.");
}

// Owns the interactive application: the keychain-backed vault, the profile
// session, the relay/transport used for first-run bootstrap, and the QML window
// currently on screen. It decides on startup between unlocking an existing
// profile (straight to chat) and running first-run onboarding, then swaps the
// onboarding surface for the chat window once the recovery code is acknowledged.
//
// Member declaration order encodes teardown order (destruction is reverse): the
// engine/window and controllers are released before the session, whose lock()
// stops networking, ahead of the transport it borrows and the relay the transport
// borrows; the vault (referenced by the session) outlives them all.
class AppRuntime final
{
public:
    AppRuntime(QString profilesRoot, OpenChat::RelayEndpoints endpoints,
               std::optional<QSslConfiguration> tls, int keyPackageCount,
               std::optional<int> width, std::optional<int> height,
               bool uglyVoiceDebug = false)
        : m_profilesRoot(std::move(profilesRoot))
        , m_endpoints(std::move(endpoints))
        , m_tls(std::move(tls))
        , m_keyPackageCount(keyPackageCount)
        , m_width(width)
        , m_height(height)
        , m_uglyVoiceDebug(uglyVoiceDebug)
    {
    }

    AppRuntime(const AppRuntime &) = delete;
    AppRuntime &operator=(const AppRuntime &) = delete;

    // Chooses the startup surface. Returns false (without showing a window) when
    // the vault is unavailable or an existing profile cannot be unlocked, so the
    // caller can exit cleanly; returns true once a window is up.
    [[nodiscard]] bool start()
    {
        if (m_vault.availability() != OpenChat::KeyVaultAvailability::Available) {
            qWarning().noquote() << QStringLiteral(
                "OpenChat: the OS keychain is unavailable; cannot open or create a "
                "profile on this system.");
            return false;
        }
        QDir().mkpath(m_profilesRoot);

        // An account from before usernames and passwords cannot be signed in to
        // any more. Erase it, and everything cached alongside it, straight away
        // and fall through to onboarding.
        if (OpenChat::hasOutdatedProfiles(m_profilesRoot)) {
            const OpenChat::LocalDataResetReport report = OpenChat::eraseOutdatedLocalData(
                m_vault, OpenChat::LocalDataLocations::standard(m_profilesRoot));
            qWarning().noquote()
                << QStringLiteral("OpenChat: erased %1 outdated local profile(s)%2.")
                       .arg(report.profilesErased)
                       .arg(report.complete()
                                ? QString()
                                : QStringLiteral("; %1 item(s) could not be removed and "
                                                 "will be retried on the next start")
                                      .arg(report.failures.size()));
            m_onboardingNotice = QStringLiteral(
                "OpenChat now signs you in with a username and password. The old account "
                "data on this computer has been erased. Create your account to continue.");
        }

        if (const std::optional<OpenChat::ProfileId> existing =
                findExistingProfile(m_profilesRoot)) {
            const OpenChat::ProfilePaths paths =
                OpenChat::ProfilePaths::forProfile(m_profilesRoot, *existing);
            auto unlocked = OpenChat::ProfileSession::unlock(*existing, m_vault, paths);
            if (!unlocked.hasValue()) {
                qWarning().noquote() << QStringLiteral(
                    "OpenChat: an existing profile could not be unlocked.");
                return false;
            }
            m_session = std::move(unlocked).value();
            // An unlocked profile holds no relay tokens: the device link
            // re-authenticates and opens the live stream (retrying with backoff
            // while offline), so restarts come back online without user action.
            enableContactServices(OpenChat::DeviceLink::Start::NeedsAuthentication);
            loadMainWindow();
            return true;
        }

        startOnboarding();
        return true;
    }

private:
    void configureWindow(QQuickWindow *window) const
    {
        if (m_width && *m_width >= OpenChat::AppMetadata::minimumWidth)
            window->setWidth(*m_width);
        if (m_height && *m_height >= OpenChat::AppMetadata::minimumHeight)
            window->setHeight(*m_height);
    }

    // Builds a fresh ChatController and loads Main into a dedicated engine. Used
    // both on the unlock path and after onboarding completes.
    void loadMainWindow()
    {
        m_chatController = std::make_unique<OpenChat::ChatController>();
        m_chatController->setConversationVisible(false);
        if (m_session) {
            QString displayName = m_session->displayName();
            // Profiles created before display names were stored have no value to
            // restore. Seed those once from the signed-in desktop account so the
            // sidebar remains useful until profile editing is available.
            if (displayName.isEmpty()) {
                displayName = qEnvironmentVariable("USER").trimmed();
                if (!displayName.isEmpty())
                    (void)m_session->setDisplayName(displayName);
            }
            m_chatController->setLocalUserName(displayName);
        }
        m_contactController = std::make_unique<OpenChat::ContactController>();
        m_callController = std::make_unique<OpenChat::CallController>();
        // Install the live seams only when the contact services came up
        // (m_contactRequests implies a live relay/session/engine). Otherwise both
        // controllers stay in their harmless mock state.
        if (m_contactRequests) {
            m_contactController->setLiveServices(m_contactRequests.get(), m_relay.get(),
                                                 m_session.get(), m_session->syncEngine());
            m_chatController->setLiveServices(m_session.get(), m_session->syncEngine(),
                                              m_contactRequests.get(), m_groups.get());
            m_chatController->setPresenceRelay(m_relay.get());
            // A resolved handle renames the chat row of an already-accepted peer,
            // and the caller shown on a ringing call screen.
            QObject::connect(m_contactController.get(),
                             &OpenChat::ContactController::contactHandleResolved,
                             m_chatController.get(),
                             [this](const QString &accountHex, const QString &) {
                                 m_chatController->refreshContact(accountHex);
                             });
            if (m_callEngine)
                m_callController->setLiveEngine(m_callEngine.get(), m_chatController.get());
        }
        // Say so when the relay stops accepting this device (logging in to the
        // account elsewhere retires it) instead of sitting silently offline, and
        // clear the banner again if a later re-check is accepted after all.
        if (m_deviceLink) {
            using SessionState = OpenChat::ChatController::SessionState;
            QObject::connect(m_deviceLink.get(), &OpenChat::DeviceLink::rejected,
                             m_chatController.get(), [this] {
                                 m_chatController->setSessionState(SessionState::SignedOut);
                             });
            QObject::connect(m_deviceLink.get(), &OpenChat::DeviceLink::linked,
                             m_chatController.get(), [this] {
                                 if (m_chatController->sessionState() == SessionState::SignedOut)
                                     m_chatController->setSessionState(SessionState::Ready);
                             });
            if (m_deviceLink->isRejected())
                m_chatController->setSessionState(SessionState::SignedOut);
        }
        m_closeToTray.reset();
        m_engine = std::make_unique<QQmlApplicationEngine>();
        // Null where the desktop has no notification area.
        m_tray = OpenChat::TrayIcon::create();
        QObject::connect(
            m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, qApp,
            [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
        m_engine->setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(m_chatController.get())},
             {QStringLiteral("dailyCaseAccount"), m_session && m_session->accountId()
                  ? m_session->accountId().value().toHex() : QStringLiteral("preview")},
             {QStringLiteral("contactController"), QVariant::fromValue(m_contactController.get())},
             {QStringLiteral("callController"), QVariant::fromValue(m_callController.get())},
             {QStringLiteral("tray"), QVariant::fromValue(m_tray.get())}});
        m_engine->loadFromModule("OpenChat", "Main");
        if (m_engine->rootObjects().isEmpty()) {
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        }
        if (auto *window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst())) {
            configureWindow(window);
            enableNotifications(window);
            // With an icon in the notification area, closing the window
            // hides it there; the icon's Close is what quits.
            if (m_tray)
                m_closeToTray = std::make_unique<OpenChat::CloseToTray>(window);
        }
        releaseFreedHeapLater(m_engine.get());

        if (m_uglyVoiceDebug) {
            m_voiceDebugController = std::make_unique<OpenChat::VoiceDebugController>();
            m_voiceDebugController->setLiveSources(
                m_callEngine.get(), m_udpCallMediaPath.get(),
                m_transportSettings.get(), m_microphoneSettings.get());
            m_debugEngine = std::make_unique<QQmlApplicationEngine>();
            m_debugEngine->setInitialProperties(
                {{QStringLiteral("debugController"), QVariant::fromValue(m_voiceDebugController.get())}});
            m_debugEngine->loadFromModule("OpenChat", "VoiceDebugWindow");
            if (!m_debugEngine->rootObjects().isEmpty()) {
                if (auto *debugWindow = qobject_cast<QQuickWindow *>(m_debugEngine->rootObjects().constFirst())) {
                    debugWindow->show();
                }
            }
        }
    }

    // Announces inbound messages on the desktop and brings the window back when
    // one is clicked.
    //
    // The service needs to know two things the controller cannot see — whether
    // the window has focus, and which conversation is on screen — so both are
    // fed to it here, and it decides for itself whether a message is worth
    // interrupting the user for.
    void enableNotifications(QQuickWindow *window)
    {
        if (m_chatController == nullptr)
            return;

        OpenChat::NotificationAppInfo appInfo;
        appInfo.applicationName = OpenChat::AppMetadata::name.toString();
        appInfo.desktopEntry = OpenChat::AppMetadata::desktopEntry.toString();
        appInfo.appUserModelId = OpenChat::AppMetadata::appUserModelId.toString();
        appInfo.iconName = OpenChat::AppMetadata::desktopEntry.toString();
        m_notifications = std::make_unique<OpenChat::NotificationService>(
            OpenChat::makeNotificationBackend(appInfo));

        QObject::connect(m_chatController.get(),
                         &OpenChat::ChatController::messageNotificationRequested,
                         m_notifications.get(),
                         [this](const QString &contactId, const QString &senderName,
                                const QString &body, const QString &avatarKey) {
                             m_notifications->postMessage(contactId, senderName, body, avatarKey);
                         });

        m_notifications->setWindowActive(window->isActive());
        QObject::connect(window, &QQuickWindow::activeChanged, m_notifications.get(),
                         [this, window] { m_notifications->setWindowActive(window->isActive()); });

        m_notifications->setActiveConversation(m_chatController->currentContactId());
        QObject::connect(m_chatController.get(), &OpenChat::ChatController::currentContactChanged,
                         m_notifications.get(), [this] {
                             m_notifications->setActiveConversation(
                                 m_chatController->currentContactId());
                         });

        // Clicking a notification is a request to read that message: bring the
        // window back (from the notification area too), leave any other
        // section, and open the conversation.
        QObject::connect(m_notifications.get(),
                         &OpenChat::NotificationService::conversationActivated, window,
                         [this, window](const QString &contactId) {
                             QMetaObject::invokeMethod(window, "bringToFront");
                             if (m_chatController == nullptr)
                                 return;
                             m_chatController->setNavSection(
                                 OpenChat::ChatController::NavSection::Chat);
                             m_chatController->selectContact(contactId);
                         });
    }

    // Brings up the durable SyncEngine over the relay transport for a live unlocked
    // session, then constructs the long-lived contact services and reconciles any
    // stashed inbound requests. Builds the relay/transport lazily (the unlock path
    // has none; the bootstrap path already made them), and is a no-op once the
    // services exist. The engine tolerates an offline relay: it queues until a link
    // comes up, so this never blocks or fails startup on connectivity. The device
    // link keeps the relay session authenticated: `linkStart` says whether the
    // bootstrap already did the first authentication or this launch must.
    void enableContactServices(OpenChat::DeviceLink::Start linkStart)
    {
        if (!m_session || m_contactRequests)
            return;
        if (!m_relay) {
            const auto account = m_session->accountId();
            const auto credential = m_session->publicCredential();
            if (!account.hasValue() || !credential.hasValue())
                return;
            m_relay = std::make_unique<OpenChat::RelayClient>(
                credential.value().deviceId, account.value(), m_endpoints,
                OpenChat::RelayCredentials{});
            if (m_tls)
                m_relay->setTlsConfiguration(*m_tls);
        }
        if (!m_transport)
            m_transport = std::make_unique<OpenChat::RelayTransport>(*m_relay);
        // Signed in: cases, the collection and what is worn come from the relay.
        OpenChat::DailyCaseController::setServiceFactory(
            [relay = QPointer<OpenChat::RelayClient>(m_relay.get())] {
                return std::make_unique<OpenChat::RelayCaseService>(relay.data());
            });
        if (!m_session->startNetworking(*m_transport).hasValue())
            return;
        OpenChat::SyncEngine *engine = m_session->syncEngine();
        if (engine == nullptr)
            return;
        // The request service self-connects to the engine's handshake signals in its
        // ctor. The SEND path is owned per-attempt by ContactController (which builds
        // a fresh AddContactService on each add), so nothing is constructed here.
        m_contactRequests =
            std::make_unique<OpenChat::ContactRequestService>(*m_session, *engine);
        m_contactRequests->reconcileOnStartup();
        // Group chats claim KeyPackages over the relay and ride the same engine.
        m_groups = std::make_unique<OpenChat::GroupService>(
            *m_session, *engine, OpenChat::GroupService::relayClaimer(*m_relay));
        m_deviceLink = std::make_unique<OpenChat::DeviceLink>(*m_session, *m_relay);
        m_deviceLink->start(linkStart);

        const auto credential = m_session->publicCredential();
        const OpenChat::DeviceId localDevice = credential.hasValue()
            ? credential.value().deviceId
            : OpenChat::DeviceId::generate();
        m_udpCallMediaPath = std::make_unique<OpenChat::UdpCallMediaPath>(localDevice);
        m_udpCallMediaPath->setRelayClient(m_relay.get());
        m_udpCallMediaPath->setSettings(m_transportSettings.get());
        const QString envMediaHost = qEnvironmentVariable("OPENCHAT_RELAY_MEDIA_HOST");
        QString relayHost = !envMediaHost.isEmpty()
            ? envMediaHost
            : (m_endpoints.live.host().isEmpty()
                ? QStringLiteral("127.0.0.1")
                : m_endpoints.live.host());
        // If connected to chat.rigidstudios.de without an explicit UDP host override,
        // send UDP packets directly to the origin server IP since Cloudflare Tunnel
        // only proxies HTTP/WebSocket over TCP.
        if (envMediaHost.isEmpty() && relayHost == QStringLiteral("chat.rigidstudios.de")) {
            relayHost = QStringLiteral("2.29.10.226");
        }
        QHostAddress relayAddress(relayHost);
        if (relayAddress.isNull()) {
            const auto hostInfo = QHostInfo::fromName(relayHost);
            if (!hostInfo.addresses().isEmpty()) {
                relayAddress = hostInfo.addresses().first();
            } else {
                relayAddress = QHostAddress(QStringLiteral("127.0.0.1"));
            }
        }
        const quint16 mediaPort = static_cast<quint16>(
            qEnvironmentVariableIntValue("OPENCHAT_RELAY_MEDIA_PORT") > 0
                ? qEnvironmentVariableIntValue("OPENCHAT_RELAY_MEDIA_PORT")
                : 8444);
        m_udpCallMediaPath->setRelayEndpoint(relayAddress, mediaPort);

        // Voice calls ride the same engine: signalling as durable MLS control
        // messages, media as unreliable datagrams. The transport tracks the live
        // link so media is dropped rather than piling up while offline.
        m_callTransport = std::make_unique<OpenChat::SyncCallTransport>(*engine);
        m_callTransport->setConnected(m_relay->isConnected());
        m_callTransport->setUdpMediaPath(m_udpCallMediaPath.get());
        QObject::connect(m_relay.get(), &OpenChat::RelayClient::connected, m_callTransport.get(),
                         [this] { m_callTransport->setConnected(true); });
        QObject::connect(m_relay.get(), &OpenChat::RelayClient::disconnected,
                         m_callTransport.get(),
                         [this] { m_callTransport->setConnected(false); });
        // A machine with no usable microphone or speaker cannot carry a call at
        // all. Leaving the engine null makes the UI report calls as unavailable
        // up front rather than letting one fail after it has started ringing.
        // Low memory mode skips the check: asking loads Qt Multimedia, which
        // it keeps unloaded until a call needs it, so there a machine without
        // audio finds out when a call starts instead.
        if (OpenChat::MemorySettings::lowMemoryModeAtStartup()
            || OpenChat::hasUsableCallAudioDevices()) {
            OpenChat::CallEngine::Config callConfig;
            // A group call keys each pair's media from both device ids.
            if (credential.hasValue())
                callConfig.localDevice = credential.value().deviceId;
            // The microphone the user picked, with their gain and gate; the
            // engine follows the settings for as long as both exist.
            callConfig.microphone = m_microphoneSettings->processing();
            m_callEngine = std::make_unique<OpenChat::CallEngine>(
                callConfig, *m_callTransport,
                OpenChat::makeQtCallAudioIoFactory(
                    [this] { return m_microphoneSettings->selectedInputDevice(); }));
            m_callEngine->setUdpMediaPath(m_udpCallMediaPath.get());
            QObject::connect(m_microphoneSettings.get(),
                             &OpenChat::MicrophoneSettings::processingChanged,
                             m_callEngine.get(), [this] {
                                 m_callEngine->setMicrophone(m_microphoneSettings->processing());
                             });
            // The custom vocal FX chain, on the same footing: the engine
            // follows what the editor has applied, and builds it per call.
            m_callEngine->setVoiceEffectFactory(m_voiceEffects->factory());
            QObject::connect(m_voiceEffects.get(),
                             &OpenChat::VoiceEffectHost::factoryChanged,
                             m_callEngine.get(), [this] {
                                 m_callEngine->setVoiceEffectFactory(m_voiceEffects->factory());
                             });
        } else {
            qWarning().noquote() << QStringLiteral(
                "OpenChat: no usable audio input/output was found; voice calls are "
                "disabled for this session.");
        }
    }

    void startOnboarding()
    {
        m_onboardingController = std::make_unique<OpenChat::OnboardingController>(
            [this](OpenChat::OnboardingController::Mode mode, const QString &handle,
                   const QString &password) { derivePasswordKeyThenBegin(mode, handle, password); });
        m_onboardingController->setNotice(m_onboardingNotice);
        // Defer the swap so it never runs inside the QML button callback that
        // emitted completed().
        QObject::connect(
            m_onboardingController.get(), &OpenChat::OnboardingController::completed,
            m_onboardingController.get(), [this] { swapToMain(); }, Qt::QueuedConnection);

        m_onboardingView = std::make_unique<QQuickView>();
        m_onboardingView->setResizeMode(QQuickView::SizeRootObjectToView);
        m_onboardingView->setTitle(OpenChat::AppMetadata::name.toString());
        m_onboardingView->setInitialProperties(
            {{QStringLiteral("controller"),
              QVariant::fromValue(m_onboardingController.get())}});
        m_onboardingView->loadFromModule("OpenChat", "Onboarding");
        if (m_onboardingView->status() == QQuickView::Error) {
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        }
        m_onboardingView->resize(OpenChat::AppMetadata::defaultWidth,
                                 OpenChat::AppMetadata::defaultHeight);
        configureWindow(m_onboardingView.get());
        m_onboardingView->show();
    }

    // The real onboarding Starter, part one. Stretching the password costs a few
    // hundred milliseconds and 64 MiB, so it runs on a pool thread; the UI keeps
    // painting its busy state meanwhile. Only the resulting key comes back to
    // this thread -- the password copy is overwritten on the worker as soon as the
    // key exists. The controller is the context object of the hand-back, so an
    // onboarding surface that is gone by then simply drops the result.
    void derivePasswordKeyThenBegin(OpenChat::OnboardingController::Mode mode,
                                    const QString &handle, const QString &password)
    {
        QPointer<OpenChat::OnboardingController> controller = m_onboardingController.get();
        QThreadPool::globalInstance()->start([this, controller, mode, handle,
                                              secret = password]() mutable {
            auto derived = OpenChat::derivePasswordKey(handle, secret);
            secret.fill(QChar(u'\0'));
            secret.clear();
            auto key = std::make_shared<std::optional<OpenChat::SecureBuffer>>();
            if (derived.hasValue())
                key->emplace(std::move(derived).value());
            if (!controller)
                return;
            QMetaObject::invokeMethod(
                controller.data(),
                [this, mode, handle, key] {
                    if (!key->has_value()) {
                        reportFailure(QStringLiteral(
                            "Couldn't prepare your password on this device. Please try again."));
                        return;
                    }
                    beginAccountFlow(mode, handle, **key);
                },
                Qt::QueuedConnection);
        });
    }

    // Part two: creates the local profile, reveals its one-time recovery code,
    // and drives an AccountBootstrap that either registers the new account or
    // logs in to the existing one. The outcome is forwarded to the controller
    // through onSubmitSucceeded()/onSubmitFailed().
    void beginAccountFlow(OpenChat::OnboardingController::Mode mode, const QString &handle,
                          const OpenChat::SecureBuffer &passwordKey)
    {
        const bool signingUp = mode == OpenChat::OnboardingController::Mode::SignUp;
        const OpenChat::ProfileId profileId = OpenChat::ProfileId::generate();
        const OpenChat::ProfilePaths paths =
            OpenChat::ProfilePaths::forProfile(m_profilesRoot, profileId);

        auto created = OpenChat::ProfileSession::create(profileId, m_vault, paths);
        if (!created.hasValue()) {
            reportFailure(QStringLiteral("Couldn't set up OpenChat on this device. Please try again."));
            return;
        }
        m_session = std::move(created).value();
        m_pendingProfileId = profileId;
        // The username doubles as the initial display name; it can be changed
        // from the profile menu afterwards.
        if (!m_session->setDisplayName(handle).hasValue()) {
            rollbackPendingProfile();
            reportFailure(QStringLiteral("Couldn't set up OpenChat on this device. Please try again."));
            return;
        }

        // Reveal the recovery code up front; it is orthogonal to the network flow.
        QString recoveryCode;
        if (auto taken = m_session->takeRecoveryCode(); taken.hasValue()) {
            OpenChat::RecoveryCode code = std::move(taken).value();
            if (auto revealed = code.reveal(); revealed.hasValue())
                recoveryCode = QString::fromLatin1(revealed.value());
        }

        const auto account = m_session->accountId();
        const auto credential = m_session->publicCredential();
        if (!account.hasValue() || !credential.hasValue()) {
            rollbackPendingProfile();
            reportFailure(QStringLiteral("Couldn't set up OpenChat on this device. Please try again."));
            return;
        }

        m_relay = std::make_unique<OpenChat::RelayClient>(
            credential.value().deviceId, account.value(), m_endpoints,
            OpenChat::RelayCredentials{});
        if (m_tls)
            m_relay->setTlsConfiguration(*m_tls);
        m_transport = std::make_unique<OpenChat::RelayTransport>(*m_relay);
        m_bootstrap = std::make_unique<OpenChat::AccountBootstrap>(*m_session, *m_relay,
                                                                   *m_transport);

        // Queued, like failed below: a storage problem here tears the bootstrap
        // down, which must not happen on its own emit stack.
        QObject::connect(
            m_bootstrap.get(), &OpenChat::AccountBootstrap::succeeded, m_bootstrap.get(),
            [this, paths, recoveryCode] {
                // Stamp the profile as a username + password account only now
                // that the relay holds it as one. A profile that never gets this
                // far has no stamp and is erased on the next start, so a crash
                // mid-flow cannot strand a half-registered profile; the account
                // itself stays reachable by logging in.
                if (!OpenChat::markProfileAccountLayoutCurrent(paths)) {
                    rollbackFailedCreation(QStringLiteral(
                        "Your account is ready, but it couldn't be saved on this device. "
                        "Log in with your username and password to try again."));
                    return;
                }
                // A login confirms the canonical username; keep the name in step.
                if (m_bootstrap && m_session)
                    (void)m_session->setDisplayName(m_bootstrap->handle());
                m_pendingProfileId.reset(); // committed and live
                if (m_onboardingController)
                    m_onboardingController->onSubmitSucceeded(recoveryCode);
            },
            Qt::QueuedConnection);
        // Queued: the failed handler tears down the networking objects, including
        // the bootstrap that emitted the signal, so it must run off the emit stack.
        QObject::connect(
            m_bootstrap.get(), &OpenChat::AccountBootstrap::failed, m_bootstrap.get(),
            [this](OpenChat::AccountBootstrap::Error error) {
                rollbackFailedCreation(messageForBootstrapError(error));
            },
            Qt::QueuedConnection);

        // The relay call needs plain bytes; they are overwritten as soon as the
        // request has been handed to the network stack.
        QByteArray keyBytes = passwordKey.view().toByteArray();
        if (signingUp)
            m_bootstrap->start(handle, keyBytes, m_keyPackageCount);
        else
            m_bootstrap->startLogin(handle, keyBytes, m_keyPackageCount);
        scrubBytes(keyBytes);
    }

    // Tears down the in-flight bootstrap networking and the just-created local
    // profile so a retry starts clean, then surfaces the message.
    void rollbackFailedCreation(const QString &message)
    {
        // The bootstrap is the failed() signal's sender; hand the QObjects to
        // deleteLater rather than destroying them inline.
        if (m_bootstrap)
            m_bootstrap.release()->deleteLater();
        if (m_transport)
            m_transport.release()->deleteLater();
        if (m_relay)
            m_relay.release()->deleteLater();
        rollbackPendingProfile();
        reportFailure(message);
    }

    // Locks and removes the pending profile (no networking is running on the
    // failure path, so lock() touches neither transport nor relay).
    void rollbackPendingProfile()
    {
        const std::optional<OpenChat::ProfileId> id = m_pendingProfileId;
        m_pendingProfileId.reset();
        m_session.reset();
        if (id) {
            const OpenChat::ProfilePaths paths =
                OpenChat::ProfilePaths::forProfile(m_profilesRoot, *id);
            (void)OpenChat::ProfileSession::removeLocalProfile(*id, m_vault, paths,
                                                               id->toHex());
        }
    }

    void reportFailure(const QString &message)
    {
        if (m_onboardingController)
            m_onboardingController->onSubmitFailed(message);
    }

    // Recovery acknowledged: bring up the chat window and retire the onboarding
    // surface on a later event-loop turn (never from inside its own callback).
    void swapToMain()
    {
        enableContactServices(OpenChat::DeviceLink::Start::AlreadyLive);
        loadMainWindow();
        if (m_onboardingView)
            m_onboardingView->hide();
        if (OpenChat::OnboardingController *controller = m_onboardingController.release())
            controller->deleteLater();
        if (QQuickView *view = m_onboardingView.release())
            view->deleteLater();
    }

    OpenChat::QtKeychainVault m_vault;
    QString m_profilesRoot;
    QString m_onboardingNotice;
    OpenChat::RelayEndpoints m_endpoints;
    std::optional<QSslConfiguration> m_tls;
    int m_keyPackageCount;
    std::optional<int> m_width;
    std::optional<int> m_height;

    std::unique_ptr<OpenChat::RelayClient> m_relay;
    std::unique_ptr<OpenChat::RelayTransport> m_transport;
    std::unique_ptr<OpenChat::ProfileSession> m_session;
    std::optional<OpenChat::ProfileId> m_pendingProfileId;
    std::unique_ptr<OpenChat::AccountBootstrap> m_bootstrap;
    // Live-session contact receive service. Declared AFTER m_session (destroyed
    // before it) and after m_transport/m_relay (destroyed before those), so it
    // disconnects from the engine while it, the session, the transport and the relay
    // are all still alive. Only populated on a live unlocked session
    // (enableContactServices).
    std::unique_ptr<OpenChat::ContactRequestService> m_contactRequests;
    // Group chats: borrows the session, engine and relay like the request
    // service, and is torn down with it.
    std::unique_ptr<OpenChat::GroupService> m_groups;
    // Keeps the relay session authenticated for the life of the profile session.
    // Declared after the relay it borrows (destroyed before it).
    std::unique_ptr<OpenChat::DeviceLink> m_deviceLink;
    // The microphone as configured in settings. Declared ahead of the call
    // engine: the engine's device chooser reads it when a call starts, so it
    // must outlive any engine.
    std::unique_ptr<OpenChat::MicrophoneSettings> m_microphoneSettings =
        std::make_unique<OpenChat::MicrophoneSettings>();
    // The scanned plugin inventory, the user's consents, and the chain they
    // have applied. Declared beside the microphone settings and ahead of the
    // engine for the same reason: the engine reads it when a call starts.
    std::unique_ptr<OpenChat::VoiceEffectHost> m_voiceEffects =
        std::make_unique<OpenChat::VoiceEffectHost>();
    // Low memory mode, the instance the Settings page switches.
    std::unique_ptr<OpenChat::MemorySettings> m_memorySettings =
        std::make_unique<OpenChat::MemorySettings>();
    std::unique_ptr<OpenChat::TransportSettings> m_transportSettings =
        std::make_unique<OpenChat::TransportSettings>();
    // The voice-call stack. Declared after the engine/relay they borrow, so both
    // are torn down while the SyncEngine and RelayClient are still alive; the
    // engine is destroyed before the transport it holds a reference to.
    std::unique_ptr<OpenChat::UdpCallMediaPath> m_udpCallMediaPath;
    std::unique_ptr<OpenChat::SyncCallTransport> m_callTransport;
    std::unique_ptr<OpenChat::CallEngine> m_callEngine;

    std::unique_ptr<OpenChat::OnboardingController> m_onboardingController;
    std::unique_ptr<OpenChat::ChatController> m_chatController;
    // Declared next to m_chatController so it tears down with the controllers, ahead
    // of m_contactRequests / m_session / m_transport / m_relay: its transient
    // AddContactService borrows the session, relay and engine by reference.
    std::unique_ptr<OpenChat::ContactController> m_contactController;
    std::unique_ptr<OpenChat::CallController> m_callController;
    std::unique_ptr<QQuickView> m_onboardingView;
    // Declared after the controllers it listens to, so it is destroyed while
    // they are still alive, and before the engine that owns the window, whose
    // connections to it are severed with the window. Its destructor takes back
    // anything the application still has showing on the desktop.
    std::unique_ptr<OpenChat::NotificationService> m_notifications;
    // Declared before the engine, whose window binds to it, so it outlives it.
    std::unique_ptr<OpenChat::TrayIcon> m_tray;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    // Declared after the engine so it lets go of the window first.
    std::unique_ptr<OpenChat::CloseToTray> m_closeToTray;
    bool m_uglyVoiceDebug = false;
    std::unique_ptr<OpenChat::VoiceDebugController> m_voiceDebugController;
    std::unique_ptr<QQmlApplicationEngine> m_debugEngine;
};

// Loads the chat window with a mock ChatController and runs the event loop. This
// is the historical --capture path, preserved so the capture smoke tests render
// the chat window unchanged.
int runChatWindow(QGuiApplication &application, QCommandLineParser &parser,
                  const QCommandLineOption &captureOption,
                  const QCommandLineOption &delayOption,
                  const QCommandLineOption &widthOption,
                  const QCommandLineOption &heightOption)
{
    OpenChat::ChatController chatController;
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("OpenChat", "Main");

    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return EXIT_FAILURE;

    applyWindowSizing(parser, window, widthOption, heightOption);
    scheduleCaptureIfRequested(parser, window, captureOption, delayOption);
    return application.exec();
}

// Loads the onboarding surface directly with a preview OnboardingController (the
// default placeholder Starter, no real services). Committable preview path used
// to launch and capture the onboarding screens.
int runOnboardingPreview(QGuiApplication &application, QCommandLineParser &parser,
                         const QCommandLineOption &captureOption,
                         const QCommandLineOption &delayOption,
                         const QCommandLineOption &widthOption,
                         const QCommandLineOption &heightOption, bool startAtRecovery,
                         bool startAtLogin, bool startFilled)
{
    OpenChat::OnboardingController controller;
    if (startAtLogin)
        controller.setMode(OpenChat::OnboardingController::Mode::LogIn);
    if (startFilled) {
        // The busiest the form gets: the startup notice, a strength reading and
        // a field hint all showing at once.
        controller.setNotice(QStringLiteral(
            "OpenChat now signs you in with a username and password. The old account "
            "data on this computer has been erased. Create your account to continue."));
        controller.setHandle(QStringLiteral("ada"));
        controller.setPassword(QStringLiteral("analytical engine no. 1"));
        controller.setPasswordConfirm(QStringLiteral("analytical engine"));
    }
    if (startAtRecovery) {
        controller.setHandle(QStringLiteral("ada"));
        controller.setPassword(QStringLiteral("analytical engine no. 1"));
        controller.setPasswordConfirm(QStringLiteral("analytical engine no. 1"));
        controller.submit(); // placeholder Starter succeeds -> Recovery
    }

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setTitle(OpenChat::AppMetadata::name.toString());
    view.setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    view.loadFromModule("OpenChat", "Onboarding");
    if (view.status() == QQuickView::Error)
        return EXIT_FAILURE;

    view.resize(OpenChat::AppMetadata::defaultWidth, OpenChat::AppMetadata::defaultHeight);
    applyWindowSizing(parser, &view, widthOption, heightOption);
    scheduleCaptureIfRequested(parser, &view, captureOption, delayOption);
    view.show();
    return application.exec();
}

// Loads the chat window with a mock ChatController and a preview ContactController
// (no real services), seeding a couple of inbound requests and a preset invite and
// opening the add-contact dialog. Committable preview path used to launch and
// capture the add-contact surface. In this phase Main.qml carries no
// contactController bindings yet, so it renders like the default chat window; the
// visible add-contact UI arrives in Phase 10b.
int runContactWindow(QGuiApplication &application, QCommandLineParser &parser,
                     const QCommandLineOption &captureOption,
                     const QCommandLineOption &delayOption,
                     const QCommandLineOption &widthOption,
                     const QCommandLineOption &heightOption)
{
    OpenChat::ChatController chatController;
    OpenChat::ContactController contactController;
    contactController.enableForPreview();
    // Seeded the way a resolved request renders: the sender's handle as the
    // title, not the account-id fallback shown while a handle is still unknown.
    contactController.addMockRequest(QStringLiteral("@ada"),
                                     QStringLiteral("wants to chat with you"));
    contactController.addMockRequest(QStringLiteral("@grace"),
                                     QStringLiteral("wants to chat with you"));
    contactController.setMockInvite(QStringLiteral("OPENCHAT-INV-9F3K-77QX-2M8D-4T1P"));
    // Preview the Search Chats and Users row too: a seeded directory handle typed into the
    // search resolves as Found with the send-request affordance.
    contactController.setMockDirectory({QStringLiteral("ada")});
    chatController.setSearchQuery(QStringLiteral("ada"));
    contactController.lookup(QStringLiteral("ada"));

    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
         {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("OpenChat", "Main");

    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return EXIT_FAILURE;

    applyWindowSizing(parser, window, widthOption, heightOption);
    scheduleCaptureIfRequested(parser, window, captureOption, delayOption);
    return application.exec();
}

// Loads the chat window with a mock ChatController and a preview ContactController
// (no real services), seeding a preset safety number and opening the
// safety-number dialog. Committable preview path used to launch and capture the
// contact-verification surface; the injected controllers stay in their harmless
// disabled mock state (no session, no network).
int runVerifyWindow(QGuiApplication &application, QCommandLineParser &parser,
                    const QCommandLineOption &captureOption,
                    const QCommandLineOption &delayOption,
                    const QCommandLineOption &widthOption,
                    const QCommandLineOption &heightOption)
{
    OpenChat::ChatController chatController;
    OpenChat::ContactController contactController;
    contactController.enableForPreview();
    contactController.setMockSafetyNumber(
        QStringLiteral("12345 67890 24680 13579 11223 44556 77889 90011 22334 45566 "
                       "77889 90011"),
        /*verified*/ false, QStringLiteral("@ada"));
    contactController.openSafetyNumberPreview();

    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
         {QStringLiteral("contactController"), QVariant::fromValue(&contactController)}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("OpenChat", "Main");

    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return EXIT_FAILURE;

    applyWindowSizing(parser, window, widthOption, heightOption);
    scheduleCaptureIfRequested(parser, window, captureOption, delayOption);
    return application.exec();
}

// Loads the chat window with a mock ChatController and a preview CallController
// pinned into an active call, so the in-call surface — two callers side by side,
// the speaker ringed in green — can be launched and captured without a peer, a
// microphone or a network. The injected controllers stay in their harmless
// disabled mock state.
int runCallWindow(QGuiApplication &application, QCommandLineParser &parser,
                  const QCommandLineOption &captureOption,
                  const QCommandLineOption &delayOption, const QCommandLineOption &widthOption,
                  const QCommandLineOption &heightOption, bool incoming, bool video, bool group,
                  bool screenShare, bool sourcePicker, bool fullscreen, bool zoom,
                  bool uglyVoiceDebug = false)
{
    OpenChat::ChatController chatController;
    chatController.setLocalUserName(QStringLiteral("Developer"));
    OpenChat::CallController callController;
    callController.setLocalIdentity(chatController.localUserName(),
                                    chatController.localAvatarKey());
    if (group) {
        // A group call mid-way through ringing: one member talking, one still
        // ringing, one who declined, so every participant state is on screen.
        OpenChat::CallParticipantRow jessica{QStringLiteral("d1"), QStringLiteral("Jessica"),
                                             QStringLiteral("jessica"), QString(), true, false,
                                             true, 0.42};
        OpenChat::CallParticipantRow michael{QStringLiteral("d2"), QStringLiteral("Michael"),
                                             QStringLiteral("michael"), QStringLiteral("Ringing…"),
                                             false, true, false, 0.0};
        OpenChat::CallParticipantRow ryan{QStringLiteral("d3"), QStringLiteral("Ryan"),
                                          QStringLiteral("ryan"), QStringLiteral("Declined"),
                                          false, false, false, 0.0};
        callController.enableForGroupPreview(OpenChat::CallState::Active,
                                             QStringLiteral("Weekend plans"),
                                             {jessica, michael, ryan});
    } else {
        // Two states worth reviewing: a call still ringing, where the answer and
        // decline pair is offered, and a live call with the far end talking, which
        // is the state the green speaking ring exists for.
        callController.enableForPreview(
            incoming ? OpenChat::CallState::Ringing : OpenChat::CallState::Active,
            QStringLiteral("Jessica"), QStringLiteral("jessica"),
            /*remoteSpeaking=*/!incoming, /*localSpeaking=*/false);
    }

    if (video) {
        QImage local(640, 360, QImage::Format_RGB32);
        QImage remote(360, 640, QImage::Format_RGB32);
        local.fill(QColor("#6192ab"));
        remote.fill(QColor("#4f7a69"));
        callController.setPreviewVideo(local, remote);
    }

    if (screenShare) {
        // A stand-in desktop rather than a real capture: the preview path never
        // opens a display, exactly as it never opens a camera.
        QImage desktop(1600, 900, QImage::Format_RGB32);
        {
            QPainter painter(&desktop);
            painter.fillRect(QRect(0, 0, 1600, 900), QColor("#20262e"));
            painter.fillRect(QRect(0, 0, 1600, 46), QColor("#39434f"));
            painter.fillRect(QRect(0, 46, 260, 854), QColor("#2a323c"));
            for (int line = 0; line < 26; ++line) {
                painter.fillRect(QRect(300, 80 + line * 30, 120 + (line * 67) % 900, 11),
                                 line % 3 == 0 ? QColor("#e2e8f0") : QColor("#96c8f0"));
            }
            for (int row = 0; row < 8; ++row)
                painter.fillRect(QRect(30, 70 + row * 34, 200, 12), QColor("#5d6b7a"));
        }
        callController.setPreviewScreenShare(
            std::make_shared<OpenChat::ScreenCanvas>(std::move(desktop)),
            QStringLiteral("Jessica"));
        // Shown arriving with sound, so the capture includes its controls.
        callController.setPreviewRemoteScreenAudio(true);
    }

    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("chatController"), QVariant::fromValue(&chatController)},
         {QStringLiteral("callController"), QVariant::fromValue(&callController)}});
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("OpenChat", "Main");

    if (engine.rootObjects().isEmpty())
        return EXIT_FAILURE;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return EXIT_FAILURE;

    applyWindowSizing(parser, window, widthOption, heightOption);
    if (sourcePicker) {
        // Opened once the window exists, so the list is built against a real
        // screen. The sources are this machine's own; nothing is captured.
        if (auto *picker = window->findChild<QObject *>(QStringLiteral("screenSharePicker")))
            QMetaObject::invokeMethod(picker, "show", Qt::QueuedConnection);
    }
    if (fullscreen)
        window->setProperty("callFullscreen", true);
    if (zoom) {
        // Enlarge the far end's share when there is one, else the far end's
        // camera: whichever the preview put on screen. Queued, so the tile has
        // a place to grow from.
        const QString tile = screenShare ? QStringLiteral("remoteScreenVideo")
                                         : QStringLiteral("remoteParticipant");
        auto *overlay = window->findChild<QObject *>(QStringLiteral("mediaZoomOverlay"));
        auto *source = window->findChild<QQuickItem *>(tile);
        if (source && !screenShare)
            source = source->findChild<QQuickItem *>(QStringLiteral("participantVideo"));
        if (overlay && source) {
            QMetaObject::invokeMethod(overlay, "enlarge", Qt::QueuedConnection,
                                      Q_ARG(QVariant, QVariant::fromValue(source)));
        }
    }
    scheduleCaptureIfRequested(parser, window, captureOption, delayOption);

    std::unique_ptr<OpenChat::VoiceDebugController> debugController;
    std::unique_ptr<QQmlApplicationEngine> debugEngine;
    if (uglyVoiceDebug) {
        debugController = std::make_unique<OpenChat::VoiceDebugController>();
        debugController->enableForPreview(&callController);
        debugEngine = std::make_unique<QQmlApplicationEngine>();
        debugEngine->setInitialProperties(
            {{QStringLiteral("debugController"), QVariant::fromValue(debugController.get())}});
        debugEngine->loadFromModule("OpenChat", "VoiceDebugWindow");
        if (!debugEngine->rootObjects().isEmpty()) {
            if (auto *dbgWin = qobject_cast<QQuickWindow *>(debugEngine->rootObjects().constFirst())) {
                dbgWin->show();
            }
        }
    }

    return application.exec();
}

// Keeps Qt Multimedia's FFmpeg backend from opening hardware codec devices.
// OpenChat never decodes or encodes video through it -- calls carry Opus,
// screen shares go through libvpx or the application's own JPEG tiles, and
// the camera only hands over frames --
// but the backend opens every device it can find (CUDA, VDPAU, VA-API, ...)
// the first time an audio device is listed, which happens at startup. That
// costs a driver thread and, measured on NVIDIA, around 11 MB for the life of
// the process. An
// empty device list turns it off; a lone comma is that list, and unlike an
// empty value it survives qputenv on Windows. A value set by the user wins.
void disableHardwareCodecProbing()
{
    for (const char *name :
         {"QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"}) {
        if (!qEnvironmentVariableIsSet(name))
            qputenv(name, ",");
    }
}

// Decided before the application object exists, from the raw arguments: the
// crash handlers are installed first thing and need to know whether this is
// the report viewer. The path itself is read later through Qt, which decodes
// the command line properly on Windows.
bool isCrashReportViewer(int argc, char *argv[])
{
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--crash-report") == 0
            || std::strncmp(argv[index], "--crash-report=", 15) == 0) {
            return true;
        }
    }
    return false;
}

// A crash report in its own window. `canRestart` is for the viewer launched
// straight after a crash, where OpenChat is not otherwise running.
std::unique_ptr<QQmlApplicationEngine> showCrashReport(const QString &path, bool canRestart)
{
    auto engine = std::make_unique<QQmlApplicationEngine>();
    auto *report = new OpenChat::CrashReportController(path, canRestart, engine.get());
    engine->setInitialProperties({{QStringLiteral("report"), QVariant::fromValue(report)}});
    engine->loadFromModule("OpenChat", "CrashNotice");
    return engine;
}

// `OpenChat --screen-share-check`: what a tester runs when sharing does not
// work, and sends back what it printed. It enumerates what the picker would
// offer, captures every screen (and one window) for a few seconds through the
// same path a call uses, and saves the first frame of each so a wrong
// orientation, a missing pointer or a black picture can be seen, not guessed.
// Window titles are never printed: they are the user's content.
int runScreenShareCheck(QGuiApplication &application)
{
    Q_UNUSED(application);
    QTextStream out(stdout);
    out << "OpenChat screen share check\n"
        << "  capture path: " << OpenChat::QtScreenCapture::backendName() << '\n'
        << "  system:       " << QSysInfo::prettyProductName() << ", "
        << QSysInfo::currentCpuArchitecture() << '\n'
        << "  Qt:           " << qVersion() << '\n';
    const bool permitted = OpenChat::NativeScreenCapturePlatform::access(false)
        == OpenChat::ScreenCaptureAccess::Granted;
    out << "  permission:   "
        << (permitted ? QStringLiteral("granted")
                      : QStringLiteral("DENIED: ")
                            + OpenChat::NativeScreenCapturePlatform::accessDeniedMessage())
        << '\n';
    out.flush();

    const QVector<OpenChat::ScreenShareSource> sources =
        OpenChat::QtScreenCapture::availableSources();
    int windows = 0;
    for (const OpenChat::ScreenShareSource &source : sources) {
        if (source.kind == OpenChat::ScreenShareSource::Kind::Window)
            ++windows;
    }
    out << "  sources:      " << (sources.size() - windows) << " screen(s), " << windows
        << " window(s)\n\n";

    const QString folder = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("openchat-screen-check"));
    QDir().mkpath(folder);
    int failures = 0;
    bool triedWindow = false;
    int ordinal = 0;
    for (const OpenChat::ScreenShareSource &source : sources) {
        const bool screen = source.kind == OpenChat::ScreenShareSource::Kind::Screen;
        if (!screen && triedWindow)
            continue;
        triedWindow = triedWindow || !screen;
        ++ordinal;
        out << (screen ? QStringLiteral("  screen \"%1\"").arg(source.name)
                       : QStringLiteral("  the first window in the list (%1x%2)")
                             .arg(source.size.width())
                             .arg(source.size.height()))
            << '\n';
        out.flush();

        OpenChat::QtScreenCapture capture;
        int frames = 0;
        QImage first;
        QString failure;
        QEventLoop loop;
        // The frames also go through the same VP9 encoder and decoder a call
        // uses, so the report says whether this machine can keep up.
        QObject codecContext;
        OpenChat::ScreenVideoEncoder encoder(&codecContext);
        OpenChat::ScreenVideoDecoder decoder(&codecContext);
        int encoded = 0;
        int decoded = 0;
        qint64 encodedBytes = 0;
        qint64 encodeUsTotal = 0;
        encoder.setOnEncoded([&](const OpenChat::EncodedScreenFrame &frame) {
            ++encoded;
            encodedBytes += frame.data.size();
            encodeUsTotal += encoder.stats().lastEncodeUs;
            decoder.submit(frame.data, frame.keyframe);
        });
        decoder.setOnPicture([&](QImage) { ++decoded; });
        QElapsedTimer codecClock;
        codecClock.start();
        const bool video = OpenChat::ScreenVideoEncoder::isAvailable();
        capture.onFrame = [&](const OpenChat::ScreenFrameView &view) {
            ++frames;
            if (first.isNull()) {
                first = QImage(view.bits, view.width, view.height, view.bytesPerLine, view.format)
                            .copy();
            }
            if (video)
                (void)encoder.submit(view, 1000 + codecClock.elapsed());
        };
        QObject::connect(&capture, &OpenChat::QtScreenCapture::failed, &loop,
                         [&](const QString &message, bool) {
                             failure = message;
                             loop.quit();
                         });
        QElapsedTimer clock;
        clock.start();
        capture.setTargetFps(30);
        capture.start(source);
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        if (failure.isEmpty())
            loop.exec();
        capture.stop();

        if (!failure.isEmpty()) {
            ++failures;
            out << "    FAILED after " << clock.elapsed() << " ms: " << failure << "\n\n";
        } else if (frames == 0) {
            ++failures;
            out << "    FAILED: no frame in 3 s\n\n";
        } else {
            const QString picture = QDir(folder).filePath(QStringLiteral("capture-%1.png").arg(ordinal));
            const bool saved = first.save(picture);
            // One colour everywhere is what a refused capture usually looks
            // like: protected content, a missing permission, a GPU mismatch.
            bool uniform = true;
            const QRgb corner = first.pixel(0, 0);
            for (int y = 0; uniform && y < first.height(); y += 16) {
                for (int x = 0; x < first.width(); x += 16) {
                    if (first.pixel(x, y) != corner) {
                        uniform = false;
                        break;
                    }
                }
            }
            out << "    ok: " << frames << " frames delivered in 3 s, first frame "
                << first.width() << 'x' << first.height() << '\n';
            if (!video) {
                out << "    VP9: not in this build (tile encoder only)\n";
            } else {
                // Let the last frames finish encoding and decoding.
                QElapsedTimer settle;
                settle.start();
                while (settle.elapsed() < 500)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                out << "    VP9: " << encoded << " frames encoded (" << encoder.stats().outputSize.width()
                    << 'x' << encoder.stats().outputSize.height() << "), average "
                    << (encoded > 0 ? QString::number(double(encodeUsTotal) / encoded / 1000.0, 'f', 1)
                                    : QStringLiteral("-"))
                    << " ms per frame, " << (encodedBytes * 8 / 3000) << " kbit/s, " << decoded
                    << " pictures decoded\n";
                const OpenChat::ScreenShareStats codecStats = encoder.stats();
                out << "    VP9 encoder: speed " << codecStats.encoderSpeed << ", busy "
                    << codecStats.encoderLoadPercent << "% of each frame's time"
                    << (codecStats.cpuLimited ? ", lowered to a smaller rung for this CPU" : "") << '\n';
                if (encoded > 0 && double(encodeUsTotal) / encoded > 30000.0) {
                    out << "    WARNING: encoding takes longer than a frame at 30 fps; shares from "
                           "this machine will run at a lower frame rate.\n";
                }
            }
            out << "    picture: " << (saved ? QDir::toNativeSeparators(picture)
                                             : QStringLiteral("(could not be saved)"))
                << '\n';
            if (uniform) {
                out << "    WARNING: the picture is a single colour (" << QColor(corner).name()
                    << "). The system may be refusing the capture even though frames arrive.\n";
            }
            out << '\n';
        }
        out.flush();
    }
    // The share's sound: the same capture a call uses, for three seconds.
    out << "  sound (" << OpenChat::ScreenAudioCapturePlatform::backendName() << ")\n";
    if (!OpenChat::ScreenAudioCapturePlatform::isSupported()) {
        out << "    not available: " << OpenChat::ScreenAudioCapturePlatform::unsupportedReason()
            << "\n\n";
    } else if (auto sound = OpenChat::ScreenAudioCapturePlatform::create({})) {
        std::atomic<int> soundFrames{0};
        std::atomic<int> loudest{0};
        sound->onFrame = [&](const OpenChat::StereoFrame &frame) {
            ++soundFrames;
            const auto *samples = reinterpret_cast<const qint16 *>(frame.constData());
            int peak = 0;
            for (qsizetype i = 0; i < frame.size() / 2; ++i)
                peak = std::max(peak, std::abs(int(samples[i])));
            int previous = loudest.load();
            while (peak > previous && !loudest.compare_exchange_weak(previous, peak)) {
            }
        };
        QString failure;
        if (!sound->start(failure)) {
            ++failures;
            out << "    FAILED: " << failure << "\n\n";
        } else {
            out << "    capturing " << sound->describe() << " for 3 s; play something to see "
                << "the level move\n";
            out.flush();
            QElapsedTimer listened;
            listened.start();
            while (listened.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            sound->stop();
            const int peak = loudest.load();
            out << "    ok: " << soundFrames.load() << " frames in 3 s (150 expected), loudest "
                << (peak > 0 ? QString::number(20.0 * std::log10(peak / 32768.0), 'f', 1)
                                   + QStringLiteral(" dBFS")
                             : QStringLiteral("silence"))
                << "\n";
            if (soundFrames.load() < 120) {
                ++failures;
                out << "    WARNING: fewer frames than expected; shared sound will stutter.\n";
            }
            out << '\n';
        }
    } else {
        ++failures;
        out << "    FAILED: this computer's sound cannot be shared\n\n";
    }
    out << (failures == 0 ? "Everything captured.\n" : "Some captures failed; see above.\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    attachParentConsole();
#endif
    // Only environment variables: safe before anything else, and must precede
    // the first use of Qt Multimedia.
    disableHardwareCodecProbing();
    // Crash reporting goes in before anything that could crash, the
    // application object included: a missing platform plugin is a fatal error
    // inside its constructor, and exactly the kind a tester cannot explain.
    // The names come first because they decide where reports are kept.
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChat"));
    OpenChat::CrashReporter::Options crashOptions;
    crashOptions.viewer = isCrashReportViewer(argc, argv);
    OpenChat::CrashReporter::install(crashOptions);

    QGuiApplication application(argc, argv);
    OpenChat::CrashReporter::attachToApplication();
    OpenChat::installFileLogging();
    // How the freedesktop desktops attribute this process: the basename of the
    // installed .desktop file. Wayland compositors use it as the app id, and
    // the notification daemons use it to find the application's name and icon
    // for the notifications posted below.
    QGuiApplication::setDesktopFileName(OpenChat::AppMetadata::desktopEntry.toString());
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/qt/qml/OpenChat/assets/icons/openchat-256.png")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("OpenChat secure chat client"));
    parser.addHelpOption();
    const QCommandLineOption captureOption(
        QStringLiteral("capture"), QStringLiteral("Save a rendered window capture to <path>."),
        QStringLiteral("path"));
    const QCommandLineOption delayOption(
        QStringLiteral("capture-delay"), QStringLiteral("Wait <milliseconds> before capture."),
        QStringLiteral("milliseconds"), QStringLiteral("500"));
    const QCommandLineOption widthOption(
        QStringLiteral("width"), QStringLiteral("Override window width."),
        QStringLiteral("pixels"));
    const QCommandLineOption heightOption(
        QStringLiteral("height"), QStringLiteral("Override window height."),
        QStringLiteral("pixels"));
    const QCommandLineOption onboardingOption(
        QStringLiteral("onboarding"),
        QStringLiteral("Launch the first-run onboarding surface directly (preview)."));
    const QCommandLineOption onboardingRecoveryOption(
        QStringLiteral("onboarding-recovery"),
        QStringLiteral("Preview the onboarding recovery-code screen."));
    const QCommandLineOption onboardingLoginOption(
        QStringLiteral("onboarding-login"),
        QStringLiteral("Preview the onboarding surface in log-in mode."));
    const QCommandLineOption onboardingFilledOption(
        QStringLiteral("onboarding-filled"),
        QStringLiteral("Preview the onboarding sign-up form filled in, with its notice and hints."));
    const QCommandLineOption addContactOption(
        QStringLiteral("add-contact"),
        QStringLiteral("Preview the add-contact surface (dialog + requests)."));
    const QCommandLineOption verifyOption(
        QStringLiteral("verify"),
        QStringLiteral("Preview the contact-verification safety-number surface."));
    const QCommandLineOption callOption(
        QStringLiteral("call"), QStringLiteral("Preview the in-call surface."));
    const QCommandLineOption callVideoOption(
        QStringLiteral("call-video"), QStringLiteral("Preview landscape and portrait video in a call."));
    const QCommandLineOption callIncomingOption(
        QStringLiteral("call-incoming"),
        QStringLiteral("Preview the in-call surface while a call is ringing."));
    const QCommandLineOption callGroupOption(
        QStringLiteral("call-group"), QStringLiteral("Preview a group call with several members."));
    const QCommandLineOption callScreenOption(
        QStringLiteral("call-screen"),
        QStringLiteral("Preview a received screen share alongside the camera."));
    const QCommandLineOption callPickerOption(
        QStringLiteral("call-picker"),
        QStringLiteral("Preview the screen-source picker, listing this machine's real sources."));
    const QCommandLineOption callFullscreenOption(
        QStringLiteral("call-fullscreen"),
        QStringLiteral("Preview the call filling the whole window (combine with --call-video "
                       "or --call-screen)."));
    const QCommandLineOption callZoomOption(
        QStringLiteral("call-zoom"),
        QStringLiteral("Preview the far end's share or camera enlarged over the window "
                       "(combine with --call-video or --call-screen)."));
    const QCommandLineOption crashReportOption(
        QStringLiteral("crash-report"),
        QStringLiteral("Show a crash report (OpenChat relaunches itself with this after a crash)."),
        QStringLiteral("path"));
    const QCommandLineOption crashTestOption(
        QStringLiteral("crash-test"),
        QStringLiteral("Fail on purpose, to see what a crash report looks like: segv, abort, "
                       "throw, qfatal, stack-overflow, pure-virtual, hang, or screen-frame "
                       "(crashes inside the next screen-share frame)."),
        QStringLiteral("kind"));
    const QCommandLineOption screenCheckOption(
        QStringLiteral("screen-share-check"),
        QStringLiteral("List what can be screen-shared on this machine, capture each screen for "
                       "a few seconds, save a picture of each, and print what happened."));
    const QCommandLineOption uglyVoiceDebugOption(
        {QStringLiteral("ugly-voice-debug"), QStringLiteral("voice-debug")},
        QStringLiteral("Launch verbose voice diagnostics & lag spike pinpointing overlay in a second window."));
    parser.addOptions({captureOption, delayOption, widthOption, heightOption, onboardingOption,
                       onboardingRecoveryOption, onboardingLoginOption,
                       onboardingFilledOption, addContactOption, verifyOption, callOption,
                       callIncomingOption, callVideoOption, callGroupOption, callScreenOption,
                       callPickerOption, callFullscreenOption, callZoomOption, crashReportOption,
                       crashTestOption, screenCheckOption, uglyVoiceDebugOption});
    parser.process(application);

    registerQmlTypes();

    // The viewer a crashed OpenChat relaunches: nothing else runs in it.
    if (parser.isSet(crashReportOption)) {
        const std::unique_ptr<QQmlApplicationEngine> viewer =
            showCrashReport(parser.value(crashReportOption), true);
        if (!viewer || viewer->rootObjects().isEmpty())
            return EXIT_FAILURE;
        return application.exec();
    }

    const QString crashTest = parser.value(crashTestOption).trimmed().toLower();
    if (!crashTest.isEmpty() && !OpenChat::CrashReporter::isSelfTestKind(crashTest)) {
        qWarning().noquote() << QStringLiteral("Unknown --crash-test kind: %1").arg(crashTest);
        return EXIT_FAILURE;
    }

    if (parser.isSet(screenCheckOption)) {
        // --crash-test screen-frame here proves a crash inside the capture
        // path is reported as one, without needing a call to share into.
        if (crashTest == QStringLiteral("screen-frame")) {
            OpenChat::CrashReporter::startSession();
            OpenChat::QtScreenCapture::crashOnNextFrameForTesting();
        }
        return runScreenShareCheck(application);
    }
    const bool uglyVoiceDebug = parser.isSet(uglyVoiceDebugOption);

    // Onboarding preview: launch the screens directly with no real services.
    const bool previewRecovery = parser.isSet(onboardingRecoveryOption);
    const bool previewLogin = parser.isSet(onboardingLoginOption);
    const bool previewFilled = parser.isSet(onboardingFilledOption);
    if (parser.isSet(onboardingOption) || previewRecovery || previewLogin || previewFilled)
        return runOnboardingPreview(application, parser, captureOption, delayOption, widthOption,
                                    heightOption, previewRecovery, previewLogin, previewFilled);

    // Add-contact preview: render the add-contact surface with a mock controller,
    // checked before the plain capture path so --add-contact --capture routes here.
    if (parser.isSet(addContactOption))
        return runContactWindow(application, parser, captureOption, delayOption, widthOption,
                                heightOption);

    // Verify preview: render the safety-number surface with a mock controller,
    // checked before the plain capture path so --verify --capture routes here.
    if (parser.isSet(verifyOption))
        return runVerifyWindow(application, parser, captureOption, delayOption, widthOption,
                               heightOption);

    // Call preview: render the in-call surface with a mock controller, checked
    // before the plain capture path so --call --capture routes here.
    const bool previewIncomingCall = parser.isSet(callIncomingOption);
    if (parser.isSet(callOption) || previewIncomingCall || parser.isSet(callVideoOption)
        || parser.isSet(callGroupOption) || parser.isSet(callScreenOption)
        || parser.isSet(callPickerOption) || parser.isSet(callFullscreenOption)
        || parser.isSet(callZoomOption))
        return runCallWindow(application, parser, captureOption, delayOption, widthOption,
                             heightOption, previewIncomingCall, parser.isSet(callVideoOption),
                             parser.isSet(callGroupOption), parser.isSet(callScreenOption),
                             parser.isSet(callPickerOption), parser.isSet(callFullscreenOption),
                             parser.isSet(callZoomOption), uglyVoiceDebug);

    // Capture path: render the chat window exactly as before.
    if (parser.isSet(captureOption))
        return runChatWindow(application, parser, captureOption, delayOption, widthOption,
                             heightOption);

    // Normal interactive launch: unlock an existing profile straight into chat, or
    // run first-run onboarding driving the real account bootstrap.
    //
    // Only this mode keeps the crash recorder on disk and relaunches after a
    // crash; the previews above run unattended, often several at once.
    OpenChat::CrashReporter::startSession();
    const QString base = [] {
        const QString value = QString::fromUtf8(qgetenv("OPENCHAT_RELAY_BASE_URL")).trimmed();
        return value.isEmpty() ? QStringLiteral("https://chat.rigidstudios.de/v1") : value;
    }();
    const QString devCaPath = QString::fromUtf8(qgetenv("OPENCHAT_DEV_CA")).trimmed();
    const QString profilesRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles");

    bool widthValid = false;
    bool heightValid = false;
    const int requestedWidth = parser.value(widthOption).toInt(&widthValid);
    const int requestedHeight = parser.value(heightOption).toInt(&heightValid);

    // Low memory mode draws without the graphics card: the GPU driver's own
    // heap and mappings are most of what the window costs. It can only be
    // chosen before the first window exists, hence the restart to switch.
    if (OpenChat::MemorySettings::lowMemoryModeAtStartup())
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    int exitCode = EXIT_SUCCESS;
    {
        AppRuntime runtime(profilesRoot, OpenChat::RelayEndpoints::fromBaseUrl(base),
                           buildDevCaTls(devCaPath),
                           OpenChat::AccountBootstrap::defaultKeyPackageCount,
                           widthValid ? std::optional<int>(requestedWidth) : std::nullopt,
                           heightValid ? std::optional<int>(requestedHeight) : std::nullopt,
                           uglyVoiceDebug);
        if (!runtime.start())
            return EXIT_FAILURE;

        // How the last session ended, if it did not end well and nobody has
        // seen the report yet: its own window, on top of whatever just opened.
        std::unique_ptr<QQmlApplicationEngine> crashNotice;
        const OpenChat::CrashReporter::PendingReport pending =
            OpenChat::CrashReporter::pendingReport();
        if (pending.kind != OpenChat::CrashReporter::PendingReport::Kind::None)
            crashNotice = showCrashReport(pending.path, false);

        if (crashTest == QStringLiteral("screen-frame")) {
            OpenChat::QtScreenCapture::crashOnNextFrameForTesting();
            OpenChat::CrashReporter::runSelfTest(crashTest);
        } else if (!crashTest.isEmpty()) {
            // Once the window is up and the recorder has something to show.
            QTimer::singleShot(1500, qApp,
                               [crashTest] { OpenChat::CrashReporter::runSelfTest(crashTest); });
        }

        exitCode = application.exec();
    }

    // Asked for from Settings to finish switching low memory mode. The profile
    // is locked and the relay link closed by now, so the new process never
    // runs alongside this one.
    if (OpenChat::MemorySettings::restartRequested())
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1));
    return exitCode;
}
