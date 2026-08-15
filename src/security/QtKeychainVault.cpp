#include "security/QtKeychainVault.h"

#include <qtkeychain/keychain.h>

#include <QByteArray>
#include <QEventLoop>

#include <openssl/crypto.h>

namespace OpenChat {
namespace {

constexpr qsizetype profileKeySize = 32;
constexpr auto serviceName = "org.openchat.OpenChat";

KeyVaultError mapError(QKeychain::Error error) {
  switch (error) {
  case QKeychain::EntryNotFound:
    return KeyVaultError::NotFound;
  case QKeychain::AccessDeniedByUser:
    return KeyVaultError::Cancelled;
  case QKeychain::AccessDenied:
    return KeyVaultError::Locked;
  case QKeychain::NoBackendAvailable:
  case QKeychain::NotImplemented:
    return KeyVaultError::Unavailable;
  case QKeychain::CouldNotDeleteEntry:
  case QKeychain::OtherError:
  case QKeychain::NoError:
    return KeyVaultError::StorageFailure;
  }
  return KeyVaultError::StorageFailure;
}

void runJob(QKeychain::Job &job) {
  QEventLoop loop;
  job.setAutoDelete(false);
  job.setInsecureFallback(false);
  QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
  job.start();
  loop.exec();
}

QString entryName(QByteArrayView purpose, const ProfileId &profileId) {
  return QString::fromLatin1(purpose.data(), purpose.size()) + u'/' +
         profileId.toHex();
}

Result<SecureBuffer, KeyVaultError>
readKey(QByteArrayView purpose, const ProfileId &profileId) {
  QKeychain::ReadPasswordJob job(QString::fromLatin1(serviceName));
  job.setKey(entryName(purpose, profileId));
  runJob(job);
  if (job.error() != QKeychain::NoError)
    return Result<SecureBuffer, KeyVaultError>::failure(mapError(job.error()));

  QByteArray bytes = job.binaryData();
  if (bytes.size() != profileKeySize) {
    if (!bytes.isEmpty())
      OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
    return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::InvalidKey);
  }

  auto key = SecureBuffer::fromBytes(bytes);
  OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
  return Result<SecureBuffer, KeyVaultError>::success(std::move(key));
}

Result<SecureBuffer, KeyVaultError>
createKey(QByteArrayView purpose, const ProfileId &profileId) {
  auto existing = readKey(purpose, profileId);
  if (existing.hasValue())
    return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
  if (existing.error() != KeyVaultError::NotFound)
    return Result<SecureBuffer, KeyVaultError>::failure(existing.error());

  SecureBuffer key;
  try {
    key = SecureBuffer::random(profileKeySize);
  } catch (...) {
    return Result<SecureBuffer, KeyVaultError>::failure(
        KeyVaultError::StorageFailure);
  }
  QKeychain::WritePasswordJob job(QString::fromLatin1(serviceName));
  job.setKey(entryName(purpose, profileId));
  QByteArray serialized = key.view().toByteArray();
  job.setBinaryData(serialized);
  runJob(job);
  job.setBinaryData({});
  OPENSSL_cleanse(serialized.data(), static_cast<std::size_t>(serialized.size()));
  if (job.error() != QKeychain::NoError)
    return Result<SecureBuffer, KeyVaultError>::failure(mapError(job.error()));
  return Result<SecureBuffer, KeyVaultError>::success(std::move(key));
}

Result<void, KeyVaultError>
deleteKey(QByteArrayView purpose, const ProfileId &profileId) {
  QKeychain::DeletePasswordJob job(QString::fromLatin1(serviceName));
  job.setKey(entryName(purpose, profileId));
  runJob(job);
  if (job.error() != QKeychain::NoError)
    return Result<void, KeyVaultError>::failure(mapError(job.error()));
  return Result<void, KeyVaultError>::success();
}

} // namespace

KeyVaultAvailability QtKeychainVault::availability() const {
  return QKeychain::isAvailable() ? KeyVaultAvailability::Available
                                  : KeyVaultAvailability::Unavailable;
}

Result<SecureBuffer, KeyVaultError>
QtKeychainVault::readProfileKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<SecureBuffer, KeyVaultError>::failure(
        KeyVaultError::Unavailable);

  return readKey("profile-key", profileId);
}

Result<SecureBuffer, KeyVaultError>
QtKeychainVault::createProfileKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<SecureBuffer, KeyVaultError>::failure(
        KeyVaultError::Unavailable);

  return createKey("profile-key", profileId);
}

Result<void, KeyVaultError>
QtKeychainVault::deleteProfileKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<void, KeyVaultError>::failure(KeyVaultError::Unavailable);

  return deleteKey("profile-key", profileId);
}

Result<SecureBuffer, KeyVaultError>
QtKeychainVault::readDeviceWrappingKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::Unavailable);
  return readKey("device-wrapping-key", profileId);
}

Result<SecureBuffer, KeyVaultError>
QtKeychainVault::createDeviceWrappingKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::Unavailable);
  return createKey("device-wrapping-key", profileId);
}

Result<void, KeyVaultError>
QtKeychainVault::deleteDeviceWrappingKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<void, KeyVaultError>::failure(KeyVaultError::Unavailable);
  return deleteKey("device-wrapping-key", profileId);
}

} // namespace OpenChat
