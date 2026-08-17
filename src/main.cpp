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
#include "app/ProfileSession.h"
#include "controllers/ChatController.h"
#include "controllers/OnboardingController.h"
#include "domain/Identifiers.h"
#include "network/RelayClient.h"
#include "network/RelayTransport.h"
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

// Builds the relay endpoint set from a base URL, e.g. https://host/v1. The live
// stream uses the same host/path over the wss (or ws) scheme.
OpenChat::RelayEndpoints buildEndpoints(const QString &base)
{
    OpenChat::RelayEndpoints endpoints;
    endpoints.accounts = QUrl(base + QStringLiteral("/accounts"));
    endpoints.authChallenge = QUrl(base + QStringLiteral("/auth/challenge"));
    endpoints.authComplete = QUrl(base + QStringLiteral("/auth/complete"));
    endpoints.authRefresh = QUrl(base + QStringLiteral("/auth/refresh"));
    endpoints.sync = QUrl(base + QStringLiteral("/sync"));
    endpoints.keyPackages = QUrl(base + QStringLiteral("/key-packages"));
    QUrl live(base + QStringLiteral("/live"));
    if (live.scheme() == QStringLiteral("https"))
        live.setScheme(QStringLiteral("wss"));
    else if (live.scheme() == QStringLiteral("http"))
        live.setScheme(QStringLiteral("ws"));
    endpoints.live = live;
    return endpoints;
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
        m_engine = std::make_unique<QQmlApplicationEngine>();
        QObject::connect(
            m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, qApp,
            [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
        m_engine->setInitialProperties(
            {{QStringLiteral("chatController"), QVariant::fromValue(m_chatController.get())}});
        m_engine->loadFromModule("OpenChat", "Main");
        if (m_engine->rootObjects().isEmpty()) {
            QCoreApplication::exit(EXIT_FAILURE);
            return;
        }
        if (auto *window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst()))
            configureWindow(window);
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
        Q_UNUSED(displayName); // Not persisted in this phase; the relay owns @handle.

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

    std::unique_ptr<OpenChat::OnboardingController> m_onboardingController;
    std::unique_ptr<OpenChat::ChatController> m_chatController;
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

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(OpenChat::AppMetadata::name.toString());
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChat"));
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/qt/qml/OpenChat/assets/icons/openchat.svg")));

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
    parser.addOptions({captureOption, delayOption, widthOption, heightOption, onboardingOption,
                       onboardingRecoveryOption});
    parser.process(application);

    registerQmlTypes();

    // Onboarding preview: launch the screens directly with no real services.
    const bool previewRecovery = parser.isSet(onboardingRecoveryOption);
    if (parser.isSet(onboardingOption) || previewRecovery)
        return runOnboardingPreview(application, parser, captureOption, delayOption, widthOption,
                                    heightOption, previewRecovery);

    // Capture path: render the chat window exactly as before.
    if (parser.isSet(captureOption))
        return runChatWindow(application, parser, captureOption, delayOption, widthOption,
                             heightOption);

    // Normal interactive launch: unlock an existing profile straight into chat, or
    // run first-run onboarding driving the real account bootstrap.
    const QString base = [] {
        const QString value = QString::fromUtf8(qgetenv("OPENCHAT_RELAY_BASE_URL")).trimmed();
        return value.isEmpty() ? QStringLiteral("https://localhost/v1") : value;
    }();
    const QString devCaPath = QString::fromUtf8(qgetenv("OPENCHAT_DEV_CA")).trimmed();
    const QString profilesRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/profiles");

    bool widthValid = false;
    bool heightValid = false;
    const int requestedWidth = parser.value(widthOption).toInt(&widthValid);
    const int requestedHeight = parser.value(heightOption).toInt(&heightValid);

    AppRuntime runtime(profilesRoot, buildEndpoints(base), buildDevCaTls(devCaPath),
                       OpenChat::AccountBootstrap::defaultKeyPackageCount,
                       widthValid ? std::optional<int>(requestedWidth) : std::nullopt,
                       heightValid ? std::optional<int>(requestedHeight) : std::nullopt);
    if (!runtime.start())
        return EXIT_FAILURE;

    return application.exec();
}
