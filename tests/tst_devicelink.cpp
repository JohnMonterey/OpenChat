#include <QtTest>

#include "app/DeviceLink.h"
#include "app/ProfileSession.h"
#include "network/RelayClient.h"
#include "relay/RelayTestSupport.h"
#include "security/KeyVault.h"

#include <QSignalSpy>
#include <QTemporaryDir>

#include <optional>

using namespace OpenChat;

namespace {

class SingleProfileVault final : public KeyVault {
public:
  KeyVaultAvailability availability() const override {
    return KeyVaultAvailability::Available;
  }
  Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &) override {
    return read(m_profileKey);
  }
  Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &) override {
    return create(m_profileKey);
  }
  Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override {
    m_profileKey.reset();
    return Result<void, KeyVaultError>::success();
  }
  Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &) override {
    return read(m_wrappingKey);
  }
  Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &) override {
    return create(m_wrappingKey);
  }
  Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &) override {
    m_wrappingKey.reset();
    return Result<void, KeyVaultError>::success();
  }

private:
  static Result<SecureBuffer, KeyVaultError> read(const std::optional<SecureBuffer> &key) {
    if (!key)
      return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
    return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
  }
  static Result<SecureBuffer, KeyVaultError> create(std::optional<SecureBuffer> &key) {
    key = SecureBuffer::random(32);
    return Result<SecureBuffer, KeyVaultError>::success(SecureBuffer::fromBytes(key->view()));
  }

  std::optional<SecureBuffer> m_profileKey;
  std::optional<SecureBuffer> m_wrappingKey;
};

const QString challengePath = QStringLiteral("/v1/auth/challenge");

} // namespace

// How an unlocked profile reacts when the relay will not take its device back.
class DeviceLinkTest final : public QObject {
  Q_OBJECT

private slots:
  void relayRefusalIsReportedOnceAndNotHammered_data()
  {
    QTest::addColumn<int>("status");
    // 403: the device was retired (the account was logged in to elsewhere).
    // 404: the relay does not know the device or the account.
    QTest::newRow("retired device") << 403;
    QTest::newRow("unknown device") << 404;
  }

  void relayRefusalIsReportedOnceAndNotHammered()
  {
    QFETCH(int, status);

    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());
    for (int i = 0; i < 4; ++i)
      server.enqueue(challengePath, {status, {}});

    QTemporaryDir directory;
    SingleProfileVault vault;
    const ProfileId profileId = ProfileId::generate();
    auto created = ProfileSession::create(
        profileId, vault, ProfilePaths::forProfile(directory.path(), profileId));
    QVERIFY(created.hasValue());
    auto session = std::move(created).value();

    RelayEndpoints endpoints;
    endpoints.authChallenge = server.url(challengePath);
    // Never reached here, but the client refuses to start the exchange unless
    // both halves of it are HTTPS.
    endpoints.authComplete = server.url(QStringLiteral("/v1/auth/complete"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));
    RelayClient client(session->publicCredential().value().deviceId,
                       session->accountId().value(), endpoints, RelayCredentials{});
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    DeviceLink link(*session, client);
    QSignalSpy rejected(&link, &DeviceLink::rejected);
    QSignalSpy failed(&link, &DeviceLink::authenticationFailed);
    QSignalSpy linked(&link, &DeviceLink::linked);

    link.start(DeviceLink::Start::NeedsAuthentication);

    QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 10000);
    QVERIFY(link.isRejected());
    QVERIFY(!link.isAuthenticated());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(linked.count(), 0);

    // An ordinary failure is retried after two seconds. A refusal is not: well
    // past that, the relay has still been asked exactly once.
    QTest::qWait(2800);
    QCOMPARE(server.requestCount(challengePath), 1);
    QCOMPARE(rejected.count(), 1);

    session->lock();
  }

  void transientFailureIsStillRetriedQuickly()
  {
    RelayTest::CertAuthority ca;
    RelayTest::FakeHttpsServer server(RelayTest::serverConfig(ca.localhostLeaf()));
    QVERIFY(server.isListening());
    for (int i = 0; i < 4; ++i)
      server.enqueue(challengePath, {503, {}});

    QTemporaryDir directory;
    SingleProfileVault vault;
    const ProfileId profileId = ProfileId::generate();
    auto created = ProfileSession::create(
        profileId, vault, ProfilePaths::forProfile(directory.path(), profileId));
    QVERIFY(created.hasValue());
    auto session = std::move(created).value();

    RelayEndpoints endpoints;
    endpoints.authChallenge = server.url(challengePath);
    // Never reached here, but the client refuses to start the exchange unless
    // both halves of it are HTTPS.
    endpoints.authComplete = server.url(QStringLiteral("/v1/auth/complete"));
    endpoints.live = QUrl(QStringLiteral("wss://localhost:1/live"));
    RelayClient client(session->publicCredential().value().deviceId,
                       session->accountId().value(), endpoints, RelayCredentials{});
    client.setTlsConfiguration(RelayTest::clientConfigTrusting(ca.caCertPem()));

    DeviceLink link(*session, client);
    QSignalSpy rejected(&link, &DeviceLink::rejected);
    link.start(DeviceLink::Start::NeedsAuthentication);

    // A relay that is merely unwell says nothing about this device: no refusal
    // is reported, and the quick backoff keeps trying.
    QTRY_VERIFY_WITH_TIMEOUT(server.requestCount(challengePath) >= 2, 10000);
    QCOMPARE(rejected.count(), 0);
    QVERIFY(!link.isRejected());

    session->lock();
  }
};

QTEST_GUILESS_MAIN(DeviceLinkTest)

#include "tst_devicelink.moc"
