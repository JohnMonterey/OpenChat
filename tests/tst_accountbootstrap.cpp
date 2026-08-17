#include "app/AccountBootstrap.h"
#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "network/RelayClient.h"
#include "network/RelayTransport.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/SqlCipherDatabase.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest/QTest>

#include <memory>
#include <optional>

using namespace OpenChat;

namespace {

// Minimal in-memory KeyVault: this environment has no OS keychain, so the
// profile's database and wrapping keys live only for the test. Mirrors the fake
// vault used by tst_profilesession.
class InMemoryVault final : public KeyVault {
public:
    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }

    Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &) override
    {
        return read(m_databaseKey);
    }
    Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &) override
    {
        return create(m_databaseKey);
    }
    Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void, KeyVaultError>::success();
    }
    Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &) override
    {
        return read(m_wrappingKey);
    }
    Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &) override
    {
        return create(m_wrappingKey);
    }
    Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void, KeyVaultError>::success();
    }

private:
    static Result<SecureBuffer, KeyVaultError> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }
    static Result<SecureBuffer, KeyVaultError> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

QString sourceDir() { return QStringLiteral(OPENCHAT_SOURCE_DIR); }
QString devCaPath() { return sourceDir() + QStringLiteral("/deploy/dev-ca/out/rootCA.crt"); }

// Runs a single query against the Compose stack's Postgres via docker compose
// exec and returns the trimmed first line of output (empty on any failure).
QString psql(const QString &sql)
{
    QProcess process;
    process.start(QStringLiteral("docker"),
                  {QStringLiteral("compose"), QStringLiteral("-f"),
                   sourceDir() + QStringLiteral("/deploy/compose.yaml"), QStringLiteral("exec"),
                   QStringLiteral("-T"), QStringLiteral("postgres"), QStringLiteral("psql"),
                   QStringLiteral("-U"), QStringLiteral("ocrelay"), QStringLiteral("-d"),
                   QStringLiteral("openchat_relay"), QStringLiteral("-tAc"), sql});
    if (!process.waitForStarted(5000))
        return {};
    if (!process.waitForFinished(15000))
        return {};
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

// True when the dev CA exists and the Caddy TLS endpoint on :443 is reachable.
bool stackReachable()
{
    if (!QFileInfo::exists(devCaPath()))
        return false;
    QTcpSocket probe;
    probe.connectToHost(QStringLiteral("localhost"), 443);
    return probe.waitForConnected(2000);
}

RelayEndpoints composeEndpoints()
{
    const QString base = QStringLiteral("https://localhost/v1");
    RelayEndpoints endpoints;
    endpoints.accounts = QUrl(base + QStringLiteral("/accounts"));
    endpoints.authChallenge = QUrl(base + QStringLiteral("/auth/challenge"));
    endpoints.authComplete = QUrl(base + QStringLiteral("/auth/complete"));
    endpoints.authRefresh = QUrl(base + QStringLiteral("/auth/refresh"));
    endpoints.sync = QUrl(base + QStringLiteral("/sync"));
    endpoints.keyPackages = QUrl(base + QStringLiteral("/key-packages"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost/v1/live"));
    return endpoints;
}

// A QSslConfiguration that ADDS the dev root CA on top of the system roots and
// keeps full peer verification. Verification is never disabled.
QSslConfiguration trustingDevCa()
{
    QSslConfiguration config = QSslConfiguration::defaultConfiguration();
    const QList<QSslCertificate> cas = QSslCertificate::fromPath(devCaPath());
    QList<QSslCertificate> roots = config.caCertificates();
    roots.append(cas);
    config.setCaCertificates(roots);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    config.setProtocol(QSsl::SecureProtocols);
    return config;
}

} // namespace

class AccountBootstrapIntegrationTest final : public QObject {
    Q_OBJECT

private slots:
    void bootstrapRegistersAuthenticatesAndPublishesOverTls();
};

void AccountBootstrapIntegrationTest::bootstrapRegistersAuthenticatesAndPublishesOverTls()
{
    if (!stackReachable())
        QSKIP("Compose relay stack not reachable at https://localhost (dev CA or :443 missing)");

    // --- A freshly created profile with an in-memory vault and temp paths. ---
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    InMemoryVault vault;
    const auto profileId = ProfileId::generate();
    const auto paths = ProfilePaths::forProfile(directory.path(), profileId);
    auto created = ProfileSession::create(profileId, vault, paths);
    QVERIFY(created.hasValue());
    auto session = std::move(created).value();

    const auto account = session->accountId();
    QVERIFY(account.hasValue());
    const auto credential = session->publicCredential();
    QVERIFY(credential.hasValue());
    const QString accountHex = account.value().bytes().toHex();
    const QString deviceHex = credential.value().deviceId.bytes().toHex();
    const QString signingKeyHex = credential.value().signingPublicKey.toHex();

    // --- A real RelayClient pointed at the Compose endpoints over TLS, trusting
    //     the dev CA (verification stays on). ---
    RelayClient client(credential.value().deviceId, account.value(), composeEndpoints(),
                       RelayCredentials{});
    client.setTlsConfiguration(trustingDevCa());
    RelayTransport transport(client);

    const int keyPackageCount = 4;
    const QString handle =
        QStringLiteral("alice-") + QUuid::createUuid().toString(QUuid::Id128).toLower();

    AccountBootstrap bootstrap(*session, client, transport);
    bool succeeded = false;
    std::optional<AccountBootstrap::Error> failure;
    connect(&bootstrap, &AccountBootstrap::succeeded, this, [&] { succeeded = true; });
    connect(&bootstrap, &AccountBootstrap::failed, this,
            [&](AccountBootstrap::Error error) { failure = error; });

    bootstrap.start(handle, keyPackageCount);

    // Drive the event loop until a terminal signal. TLS + three HTTPS round trips
    // plus N publishes should complete well under the timeout.
    QTRY_VERIFY_WITH_TIMEOUT(succeeded || failure.has_value(), 30000);
    if (failure.has_value())
        QFAIL(qPrintable(QStringLiteral("bootstrap failed with error %1")
                             .arg(static_cast<int>(*failure))));
    QVERIFY(succeeded);

    // --- Proof over real TLS: the rows the relay wrote to Postgres. ---
    // accounts: exactly one row for this handle.
    QCOMPARE(psql(QStringLiteral("SELECT count(*) FROM accounts WHERE handle = '%1'").arg(handle)),
             QStringLiteral("1"));
    // devices: exactly one row, bound to this account, carrying this signing key.
    QCOMPARE(psql(QStringLiteral("SELECT count(*) FROM devices WHERE device_id = decode('%1','hex') "
                                 "AND account_id = decode('%2','hex')")
                      .arg(deviceHex, accountHex)),
             QStringLiteral("1"));
    QCOMPARE(psql(QStringLiteral("SELECT encode(signing_key,'hex') FROM devices "
                                 "WHERE device_id = decode('%1','hex')")
                      .arg(deviceHex)),
             signingKeyHex);
    // key_packages: exactly N rows for this device/account.
    QCOMPARE(psql(QStringLiteral("SELECT count(*) FROM key_packages "
                                 "WHERE device_id = decode('%1','hex') "
                                 "AND account_id = decode('%2','hex')")
                      .arg(deviceHex, accountHex)),
             QString::number(keyPackageCount));

    // Tear down networking while the transport and client are still alive: the
    // session's SyncEngine borrows both.
    client.disconnect();
    session->lock();
}

QTEST_MAIN(AccountBootstrapIntegrationTest)
#include "tst_accountbootstrap.moc"
