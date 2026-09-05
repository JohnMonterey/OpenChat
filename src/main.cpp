#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickView>
#include <QQuickWindow>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <qqml.h>

#include <algorithm>
#include <memory>
#include <optional>

#include "app/AccountBootstrap.h"
#include "app/AppMetadata.h"
#include "app/ContactRequestService.h"
#include "app/DeviceLink.h"
#include "app/ProfileSession.h"
#include "call/CallEngine.h"
#include "call/QtAudioIo.h"
#include "call/SyncCallTransport.h"
#include "controllers/CallController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/OnboardingController.h"
#include "domain/Identifiers.h"
#include "network/RelayClient.h"
#include "network/RelayTransport.h"
#include "network/SyncEngine.h"
#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"
#include "security/KeyVault.h"
#include "security/QtKeychainVault.h"
#include "security/RecoveryCode.h"

namespace {

// Registers the C++ types the QML surfaces consume. Registration is global to the
// process, so every engine and view created below resolves the same types.
void registerQmlTypes()
{
    qmlRegisterType<OpenChat::BubbleBackground>("OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<OpenChat::AvatarArtwork>("OpenChat.Native", 1, 0, "AvatarArtwork");
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
// only when the name round-trips to a valid ProfileId and holds a profile.sqlite3.
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
        if (QFileInfo(paths.database).isFile())
            return id;
    }
    return std::nullopt;
}

QString messageForBootstrapError(OpenChat::AccountBootstrap::Error error)
{
    switch (error) {
    case OpenChat::AccountBootstrap::Error::HandleUnavailable:
        return QStringLiteral("That handle is unavailable. Please choose another.");
    case OpenChat::AccountBootstrap::Error::Auth:
        return QStringLiteral("Couldn't verify this device. Please try again.");
    case OpenChat::AccountBootstrap::Error::Publish:
    case OpenChat::AccountBootstrap::Error::Storage:
        return QStringLiteral("Couldn't finish setting up your account. Please try again.");
    case OpenChat::AccountBootstrap::Error::Transport:
        return QStringLiteral("Couldn't reach OpenChat. Check your connection and try again.");
    }
    return QStringLiteral("Couldn't create your profile. Please try again.");
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
               std::optional<int> width, std::optional<int> height)
        : m_profilesRoot(std::move(profilesRoot))
        , m_endpoints(std::move(endpoints))
        , m_tls(std::move(tls))
        , m_keyPackageCount(keyPackageCount)
        , m_width(width)
        , m_height(height)
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
                                              m_contactRequests.get());
            m_chatController->setPresenceRelay(m_relay.get());
            // A resolved handle renames the chat row of an already-accepted peer,
            // and the caller shown on a ringing call screen.
            QObject::connect(m_contactController.get(),
                             &OpenChat::ContactController::contactHandleResolved,
                             m_chatController.get(),
                             [this](const QString &accountHex, const QString &handle) {
                                 m_chatController->refreshContact(accountHex);
                                 if (m_callEngine
                                     && m_callEngine->peer().contactId == accountHex)
                                     m_callEngine->updatePeerIdentity(handle, QString());
                             });
            if (m_callEngine)
                m_callController->setLiveEngine(m_callEngine.get(), m_chatController.get());
        }
        m_engine = std::make_unique<QQmlApplicationEngine>();
        QObject::connect(
            m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, qApp,
            [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
        m_engine->setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(m_chatController.get())},
             {QStringLiteral("contactController"), QVariant::fromValue(m_contactController.get())},
             {QStringLiteral("callController"), QVariant::fromValue(m_callController.get())}});
        m_engine->loadFromModule("OpenChat", "Main");
        if (m_engine->rootObjects().isEmpty()) {
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        }
        if (auto *window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst()))
            configureWindow(window);
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
        m_deviceLink = std::make_unique<OpenChat::DeviceLink>(*m_session, *m_relay);
        m_deviceLink->start(linkStart);

        // Voice calls ride the same engine: signalling as durable MLS control
        // messages, media as unreliable datagrams. The transport tracks the live
        // link so media is dropped rather than piling up while offline.
        m_callTransport = std::make_unique<OpenChat::SyncCallTransport>(*engine);
        m_callTransport->setConnected(m_relay->isConnected());
        QObject::connect(m_relay.get(), &OpenChat::RelayClient::connected, m_callTransport.get(),
                         [this] { m_callTransport->setConnected(true); });
        QObject::connect(m_relay.get(), &OpenChat::RelayClient::disconnected,
                         m_callTransport.get(),
                         [this] { m_callTransport->setConnected(false); });
        // A machine with no usable microphone or speaker cannot carry a call at
        // all. Leaving the engine null makes the UI report calls as unavailable
        // up front rather than letting one fail after it has started ringing.
        if (OpenChat::hasUsableCallAudioDevices()) {
            m_callEngine = std::make_unique<OpenChat::CallEngine>(
                OpenChat::CallEngine::Config{}, *m_callTransport,
                OpenChat::makeQtCallAudioIoFactory());
        } else {
            qWarning().noquote() << QStringLiteral(
                "OpenChat: no usable audio input/output was found; voice calls are "
                "disabled for this session.");
        }
    }

    void startOnboarding()
    {
        m_onboardingController = std::make_unique<OpenChat::OnboardingController>(
            [this](const QString &displayName, const QString &handle) {
                beginAccountCreation(displayName, handle);
            });
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

    // The real onboarding Starter: creates the profile, reveals its one-time
    // recovery code, and drives an AccountBootstrap. Its outcome is forwarded to
    // the controller through onCreationSucceeded()/onCreationFailed().
    void beginAccountCreation(const QString &displayName, const QString &handle)
    {
        const OpenChat::ProfileId profileId = OpenChat::ProfileId::generate();
        const OpenChat::ProfilePaths paths =
            OpenChat::ProfilePaths::forProfile(m_profilesRoot, profileId);

        auto created = OpenChat::ProfileSession::create(profileId, m_vault, paths);
        if (!created.hasValue()) {
            reportFailure(QStringLiteral("Couldn't create your profile. Please try again."));
            return;
        }
        m_session = std::move(created).value();
        m_pendingProfileId = profileId;
        if (!m_session->setDisplayName(displayName).hasValue()) {
            rollbackPendingProfile();
            reportFailure(QStringLiteral("Couldn't create your profile. Please try again."));
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
            reportFailure(QStringLiteral("Couldn't create your profile. Please try again."));
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

        QObject::connect(m_bootstrap.get(), &OpenChat::AccountBootstrap::succeeded,
                         m_bootstrap.get(), [this, recoveryCode] {
                             m_pendingProfileId.reset(); // committed and live
                             if (m_onboardingController)
                                 m_onboardingController->onCreationSucceeded(recoveryCode);
                         });
        // Queued: the failed handler tears down the networking objects, including
        // the bootstrap that emitted the signal, so it must run off the emit stack.
        QObject::connect(
            m_bootstrap.get(), &OpenChat::AccountBootstrap::failed, m_bootstrap.get(),
            [this](OpenChat::AccountBootstrap::Error error) {
                rollbackFailedCreation(messageForBootstrapError(error));
            },
            Qt::QueuedConnection);

        m_bootstrap->start(handle, m_keyPackageCount);
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
            m_onboardingController->onCreationFailed(message);
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
    // Keeps the relay session authenticated for the life of the profile session.
    // Declared after the relay it borrows (destroyed before it).
    std::unique_ptr<OpenChat::DeviceLink> m_deviceLink;
    // The voice-call stack. Declared after the engine/relay they borrow, so both
    // are torn down while the SyncEngine and RelayClient are still alive; the
    // engine is destroyed before the transport it holds a reference to.
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
    std::unique_ptr<QQmlApplicationEngine> m_engine;
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
                         const QCommandLineOption &heightOption, bool startAtRecovery)
{
    OpenChat::OnboardingController controller;
    if (startAtRecovery) {
        controller.setDisplayName(QStringLiteral("Ada Lovelace"));
        controller.setHandle(QStringLiteral("ada"));
        controller.createProfile(); // placeholder Starter succeeds -> Recovery
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
    // Preview the Search & Find row too: a seeded directory handle typed into the
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
                  const QCommandLineOption &heightOption, bool incoming)
{
    OpenChat::ChatController chatController;
    chatController.setLocalUserName(QStringLiteral("Developer"));
    OpenChat::CallController callController;
    callController.setLocalIdentity(chatController.localUserName(),
                                    chatController.localAvatarKey());
    // Two states worth reviewing: a call still ringing, where the answer and
    // decline pair is offered, and a live call with the far end talking, which
    // is the state the green speaking ring exists for.
    callController.enableForPreview(
        incoming ? OpenChat::CallState::Ringing : OpenChat::CallState::Active,
        QStringLiteral("Jessica"), QStringLiteral("jessica"),
        /*remoteSpeaking=*/!incoming, /*localSpeaking=*/false);

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
    scheduleCaptureIfRequested(parser, window, captureOption, delayOption);
    return application.exec();
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChat"));
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/qt/qml/OpenChat/assets/icons/openchat.png")));

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
    const QCommandLineOption addContactOption(
        QStringLiteral("add-contact"),
        QStringLiteral("Preview the add-contact surface (dialog + requests)."));
    const QCommandLineOption verifyOption(
        QStringLiteral("verify"),
        QStringLiteral("Preview the contact-verification safety-number surface."));
    const QCommandLineOption callOption(
        QStringLiteral("call"), QStringLiteral("Preview the in-call surface."));
    const QCommandLineOption callIncomingOption(
        QStringLiteral("call-incoming"),
        QStringLiteral("Preview the in-call surface while a call is ringing."));
    parser.addOptions({captureOption, delayOption, widthOption, heightOption, onboardingOption,
                       onboardingRecoveryOption, addContactOption, verifyOption, callOption,
                       callIncomingOption});
    parser.process(application);

    registerQmlTypes();

    // Onboarding preview: launch the screens directly with no real services.
    const bool previewRecovery = parser.isSet(onboardingRecoveryOption);
    if (parser.isSet(onboardingOption) || previewRecovery)
        return runOnboardingPreview(application, parser, captureOption, delayOption, widthOption,
                                    heightOption, previewRecovery);

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
    if (parser.isSet(callOption) || previewIncomingCall)
        return runCallWindow(application, parser, captureOption, delayOption, widthOption,
                             heightOption, previewIncomingCall);

    // Capture path: render the chat window exactly as before.
    if (parser.isSet(captureOption))
        return runChatWindow(application, parser, captureOption, delayOption, widthOption,
                             heightOption);

    // Normal interactive launch: unlock an existing profile straight into chat, or
    // run first-run onboarding driving the real account bootstrap.
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

    AppRuntime runtime(profilesRoot, OpenChat::RelayEndpoints::fromBaseUrl(base),
                       buildDevCaTls(devCaPath),
                       OpenChat::AccountBootstrap::defaultKeyPackageCount,
                       widthValid ? std::optional<int>(requestedWidth) : std::nullopt,
                       heightValid ? std::optional<int>(requestedHeight) : std::nullopt);
    if (!runtime.start())
        return EXIT_FAILURE;

    return application.exec();
}
