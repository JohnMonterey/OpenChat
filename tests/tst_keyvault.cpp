#include "security/KeyVault.h"

#include <QtTest/QTest>

#include <optional>
#include <type_traits>

using namespace OpenChat;

class FakeKeyVault final : public KeyVault {
public:
  explicit FakeKeyVault(KeyVaultAvailability availability)
      : m_availability(availability) {}

  KeyVaultAvailability availability() const override { return m_availability; }

  Result<SecureBuffer, KeyVaultError>
  readProfileKey(const ProfileId &) override {
    if (m_availability != KeyVaultAvailability::Available)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::Unavailable);
    if (!m_key.has_value())
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::NotFound);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(m_key->view()));
  }

  Result<SecureBuffer, KeyVaultError>
  createProfileKey(const ProfileId &) override {
    if (m_availability != KeyVaultAvailability::Available)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::Unavailable);
    if (m_key.has_value())
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::AlreadyExists);
    m_key = SecureBuffer::random(32);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(m_key->view()));
  }

  Result<void, KeyVaultError> deleteProfileKey(const ProfileId &) override {
    if (m_availability != KeyVaultAvailability::Available)
      return Result<void, KeyVaultError>::failure(KeyVaultError::Unavailable);
    m_key.reset();
    return Result<void, KeyVaultError>::success();
  }

  Result<SecureBuffer, KeyVaultError>
  readDeviceWrappingKey(const ProfileId &) override {
    if (m_availability != KeyVaultAvailability::Available)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::Unavailable);
    if (!m_wrappingKey.has_value())
      return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(m_wrappingKey->view()));
  }

  Result<SecureBuffer, KeyVaultError>
  createDeviceWrappingKey(const ProfileId &) override {
    if (m_availability != KeyVaultAvailability::Available)
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::Unavailable);
    if (m_wrappingKey.has_value())
      return Result<SecureBuffer, KeyVaultError>::failure(
          KeyVaultError::AlreadyExists);
    m_wrappingKey = SecureBuffer::random(32);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(m_wrappingKey->view()));
  }

  Result<void, KeyVaultError>
  deleteDeviceWrappingKey(const ProfileId &) override {
    if (m_availability != KeyVaultAvailability::Available)
      return Result<void, KeyVaultError>::failure(KeyVaultError::Unavailable);
    m_wrappingKey.reset();
    return Result<void, KeyVaultError>::success();
  }

private:
  KeyVaultAvailability m_availability;
  std::optional<SecureBuffer> m_key;
  std::optional<SecureBuffer> m_wrappingKey;
};

class KeyVaultTest : public QObject {
  Q_OBJECT

private slots:
  void unavailableVaultNeverReturnsAReplacementKey();
  void missingKeyIsDistinctFromUnavailableVault();
  void secureBufferIsMoveOnlyAndSized();
  void createNeverReplacesAnExistingKey();
};

void KeyVaultTest::unavailableVaultNeverReturnsAReplacementKey() {
  FakeKeyVault vault(KeyVaultAvailability::Unavailable);
  auto result = vault.readProfileKey(ProfileId::generate());
  QVERIFY(!result.hasValue());
  QCOMPARE(result.error(), KeyVaultError::Unavailable);
}

void KeyVaultTest::missingKeyIsDistinctFromUnavailableVault() {
  FakeKeyVault vault(KeyVaultAvailability::Available);
  auto result = vault.readProfileKey(ProfileId::generate());
  QVERIFY(!result.hasValue());
  QCOMPARE(result.error(), KeyVaultError::NotFound);
}

void KeyVaultTest::secureBufferIsMoveOnlyAndSized() {
  static_assert(!std::is_copy_constructible_v<SecureBuffer>);
  static_assert(!std::is_copy_assignable_v<SecureBuffer>);
  static_assert(std::is_move_constructible_v<SecureBuffer>);

  auto key = SecureBuffer::random(32);
  QCOMPARE(key.size(), 32);
  QVERIFY(!key.isEmpty());

  auto moved = std::move(key);
  QCOMPARE(moved.size(), 32);
  QVERIFY(key.isEmpty());
}

void KeyVaultTest::createNeverReplacesAnExistingKey() {
  FakeKeyVault vault(KeyVaultAvailability::Available);
  const auto profileId = ProfileId::generate();
  auto first = vault.createProfileKey(profileId);
  QVERIFY(first.hasValue());
  auto second = vault.createProfileKey(profileId);
  QVERIFY(!second.hasValue());
  QCOMPARE(second.error(), KeyVaultError::AlreadyExists);
}

QTEST_GUILESS_MAIN(KeyVaultTest)
#include "tst_keyvault.moc"
