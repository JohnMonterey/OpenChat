#include "security/QtKeychainVault.h"

#include <qtkeychain/keychain.h>

#include <QByteArray>
#include <QEventLoop>

#include <openssl/crypto.h>

namespace OpenChat {
namespace {

constexpr qsizetype profileKeySize = 32;

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

QString entryName(const ProfileId &profileId) {
  return QStringLiteral("profile-key/") + profileId.toHex();
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

  QKeychain::ReadPasswordJob job(QString::fromLatin1(serviceName));
  job.setKey(entryName(profileId));
  runJob(job);
  if (job.error() != QKeychain::NoError)
    return Result<SecureBuffer, KeyVaultError>::failure(mapError(job.error()));

  QByteArray bytes = job.binaryData();
  if (bytes.size() != profileKeySize) {
    if (!bytes.isEmpty())
      OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
    return Result<SecureBuffer, KeyVaultError>::failure(
        KeyVaultError::InvalidKey);
  }

  auto key = SecureBuffer::fromBytes(bytes);
  OPENSSL_cleanse(bytes.data(), static_cast<std::size_t>(bytes.size()));
  return Result<SecureBuffer, KeyVaultError>::success(std::move(key));
}

Result<SecureBuffer, KeyVaultError>
QtKeychainVault::createProfileKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<SecureBuffer, KeyVaultError>::failure(
        KeyVaultError::Unavailable);

  auto existing = readProfileKey(profileId);
  if (existing.hasValue())
    return Result<SecureBuffer, KeyVaultError>::failure(
        KeyVaultError::AlreadyExists);
  if (existing.error() != KeyVaultError::NotFound)
    return Result<SecureBuffer, KeyVaultError>::failure(existing.error());

  auto key = SecureBuffer::random(profileKeySize);
  QKeychain::WritePasswordJob job(QString::fromLatin1(serviceName));
  job.setKey(entryName(profileId));
  job.setBinaryData(key.view().toByteArray());
  runJob(job);
  if (job.error() != QKeychain::NoError)
    return Result<SecureBuffer, KeyVaultError>::failure(mapError(job.error()));

  return Result<SecureBuffer, KeyVaultError>::success(std::move(key));
}

Result<void, KeyVaultError>
QtKeychainVault::deleteProfileKey(const ProfileId &profileId) {
  if (availability() != KeyVaultAvailability::Available)
    return Result<void, KeyVaultError>::failure(KeyVaultError::Unavailable);

  QKeychain::DeletePasswordJob job(QString::fromLatin1(serviceName));
  job.setKey(entryName(profileId));
  runJob(job);
  if (job.error() != QKeychain::NoError)
    return Result<void, KeyVaultError>::failure(mapError(job.error()));
  return Result<void, KeyVaultError>::success();
}

} // namespace OpenChat
