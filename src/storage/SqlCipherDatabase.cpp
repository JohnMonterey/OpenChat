#include "storage/SqlCipherDatabase.h"

#include <QFile>
#include <QFileInfo>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <sqlite3.h>

#include <memory>
#include <utility>

namespace OpenChat {
namespace {

constexpr qsizetype profileKeySize = 32;
constexpr qsizetype devicePrivateKeySize = 32;
constexpr qsizetype devicePublicKeySize = 32;
constexpr qsizetype wrappingKeySize = 32;
constexpr qsizetype nonceSize = 12;
constexpr qsizetype tagSize = 16;
constexpr qsizetype maximumMlsStateSize = qsizetype{8} * 1024 * 1024;

using CipherContextPointer =
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

QByteArray identityAad(const ProfileId &profileId, const DeviceId &deviceId,
                       QByteArrayView publicKey) {
  QByteArray aad("OpenChat wrapped device identity v1", 35);
  aad.append(profileId.bytes());
  aad.append(deviceId.bytes());
  aad.append(publicKey.data(), publicKey.size());
  return aad;
}

struct WrappedSecret final {
  QByteArray nonce;
  QByteArray ciphertext;
  QByteArray tag;
};

Result<WrappedSecret, StorageError>
wrapSecret(const SecureBuffer &plaintext, const SecureBuffer &key,
           QByteArrayView aad) {
  if (key.size() != wrappingKeySize || plaintext.isEmpty())
    return Result<WrappedSecret, StorageError>::failure(StorageError::InvalidKey);
  WrappedSecret wrapped{QByteArray(nonceSize, Qt::Uninitialized),
                        QByteArray(plaintext.size(), Qt::Uninitialized),
                        QByteArray(tagSize, Qt::Uninitialized)};
  if (RAND_bytes(reinterpret_cast<unsigned char *>(wrapped.nonce.data()),
                 static_cast<int>(wrapped.nonce.size())) != 1)
    return Result<WrappedSecret, StorageError>::failure(StorageError::QueryFailed);

  CipherContextPointer context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  int written = 0;
  int total = 0;
  if (!context ||
      EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(wrapped.nonce.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(
          context.get(), nullptr, nullptr,
          reinterpret_cast<const unsigned char *>(key.view().data()),
          reinterpret_cast<const unsigned char *>(wrapped.nonce.constData())) != 1 ||
      EVP_EncryptUpdate(context.get(), nullptr, &written,
                        reinterpret_cast<const unsigned char *>(aad.data()),
                        static_cast<int>(aad.size())) != 1 ||
      EVP_EncryptUpdate(
          context.get(),
          reinterpret_cast<unsigned char *>(wrapped.ciphertext.data()), &written,
          reinterpret_cast<const unsigned char *>(plaintext.view().data()),
          static_cast<int>(plaintext.size())) != 1)
    return Result<WrappedSecret, StorageError>::failure(StorageError::QueryFailed);
  total = written;
  if (EVP_EncryptFinal_ex(
          context.get(),
          reinterpret_cast<unsigned char *>(wrapped.ciphertext.data()) + total,
          &written) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
                          static_cast<int>(wrapped.tag.size()),
                          wrapped.tag.data()) != 1)
    return Result<WrappedSecret, StorageError>::failure(StorageError::QueryFailed);
  wrapped.ciphertext.resize(total + written);
  return Result<WrappedSecret, StorageError>::success(std::move(wrapped));
}

Result<SecureBuffer, StorageError>
unwrapSecret(QByteArrayView ciphertext, QByteArrayView nonce, QByteArrayView tag,
             const SecureBuffer &key, QByteArrayView aad) {
  if (key.size() != wrappingKeySize || ciphertext.isEmpty() ||
      nonce.size() != nonceSize || tag.size() != tagSize)
    return Result<SecureBuffer, StorageError>::failure(
        StorageError::AuthenticationFailed);
  QByteArray plaintext(ciphertext.size(), Qt::Uninitialized);
  CipherContextPointer context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  int written = 0;
  int total = 0;
  const bool initialized =
      context &&
      EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) == 1 &&
      EVP_DecryptInit_ex(
          context.get(), nullptr, nullptr,
          reinterpret_cast<const unsigned char *>(key.view().data()),
          reinterpret_cast<const unsigned char *>(nonce.data())) == 1 &&
      EVP_DecryptUpdate(context.get(), nullptr, &written,
                        reinterpret_cast<const unsigned char *>(aad.data()),
                        static_cast<int>(aad.size())) == 1 &&
      EVP_DecryptUpdate(
          context.get(), reinterpret_cast<unsigned char *>(plaintext.data()),
          &written,
          reinterpret_cast<const unsigned char *>(ciphertext.data()),
          static_cast<int>(ciphertext.size())) == 1;
  total = written;
  if (!initialized ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(tag.size()),
                          const_cast<char *>(tag.data())) != 1 ||
      EVP_DecryptFinal_ex(
          context.get(),
          reinterpret_cast<unsigned char *>(plaintext.data()) + total,
          &written) != 1) {
    OPENSSL_cleanse(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
    return Result<SecureBuffer, StorageError>::failure(
        StorageError::AuthenticationFailed);
  }
  plaintext.resize(total + written);
  auto secret = SecureBuffer::fromBytes(plaintext);
  OPENSSL_cleanse(plaintext.data(), static_cast<std::size_t>(plaintext.size()));
  return Result<SecureBuffer, StorageError>::success(std::move(secret));
}

void removeNewDatabaseFiles(const QString &path) {
  QFile::remove(path);
  QFile::remove(path + QStringLiteral("-wal"));
  QFile::remove(path + QStringLiteral("-shm"));
}

Result<QByteArray, StorageError> readMigration(const QString &resourcePath) {
  QFile file(resourcePath);
  if (!file.open(QIODevice::ReadOnly))
    return Result<QByteArray, StorageError>::failure(
        StorageError::MigrationFailed);
  return Result<QByteArray, StorageError>::success(file.readAll());
}

} // namespace

SqlCipherDatabase::SqlCipherDatabase(sqlite3 *database, QString path)
    : m_database(database), m_path(std::move(path)),
      m_mutex(std::make_unique<QRecursiveMutex>()) {}

SqlCipherDatabase::~SqlCipherDatabase() { close(); }

SqlCipherDatabase::SqlCipherDatabase(SqlCipherDatabase &&other) noexcept
    : m_database(std::exchange(other.m_database, nullptr)),
      m_path(std::move(other.m_path)), m_mutex(std::move(other.m_mutex)) {}

SqlCipherDatabase &
SqlCipherDatabase::operator=(SqlCipherDatabase &&other) noexcept {
  if (this == &other)
    return *this;
  close();
  m_database = std::exchange(other.m_database, nullptr);
  m_path = std::move(other.m_path);
  m_mutex = std::move(other.m_mutex);
  return *this;
}

Result<SqlCipherDatabase, StorageError>
SqlCipherDatabase::open(const QString &path, const SecureBuffer &key) {
  if (key.size() != profileKeySize)
    return Result<SqlCipherDatabase, StorageError>::failure(
        StorageError::InvalidKey);

  const bool existed = QFileInfo::exists(path);
  sqlite3 *handle = nullptr;
  const QByteArray nativePath = QFile::encodeName(path);
  const int flags =
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(nativePath.constData(), &handle, flags, nullptr) !=
      SQLITE_OK) {
    if (handle)
      sqlite3_close_v2(handle);
    return Result<SqlCipherDatabase, StorageError>::failure(
        StorageError::CannotOpen);
  }

  SqlCipherDatabase database(handle, path);
  if (sqlite3_key(handle, key.view().data(), static_cast<int>(key.size())) !=
      SQLITE_OK) {
    database.close();
    if (!existed)
      removeNewDatabaseFiles(path);
    return Result<SqlCipherDatabase, StorageError>::failure(
        StorageError::InvalidKey);
  }

  auto configured = database.configure();
  if (!configured.hasValue()) {
    database.close();
    if (!existed)
      removeNewDatabaseFiles(path);
    return Result<SqlCipherDatabase, StorageError>::failure(
        existed ? StorageError::WrongKeyOrCorrupt : configured.error());
  }

  auto migrated = database.migrate();
  if (!migrated.hasValue()) {
    database.close();
    if (!existed)
      removeNewDatabaseFiles(path);
    return Result<SqlCipherDatabase, StorageError>::failure(migrated.error());
  }

  return Result<SqlCipherDatabase, StorageError>::success(std::move(database));
}

Result<void, StorageError> SqlCipherDatabase::configure() {
  auto result = execute("PRAGMA cipher_memory_security = ON;"
                        "PRAGMA foreign_keys = ON;"
                        "PRAGMA secure_delete = ON;"
                        "PRAGMA temp_store = MEMORY;"
                        "PRAGMA synchronous = FULL;");
  if (!result.hasValue())
    return Result<void, StorageError>::failure(StorageError::WrongKeyOrCorrupt);

  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(m_database, "SELECT count(*) FROM sqlite_master", -1,
                         &statement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::WrongKeyOrCorrupt);
  const int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (step != SQLITE_ROW)
    return Result<void, StorageError>::failure(StorageError::WrongKeyOrCorrupt);

  if (!verifyCipherIntegrity().hasValue())
    return Result<void, StorageError>::failure(StorageError::WrongKeyOrCorrupt);

  if (!execute("PRAGMA journal_mode = WAL;").hasValue())
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  return Result<void, StorageError>::success();
}

Result<void, StorageError> SqlCipherDatabase::migrate() {
  sqlite3_stmt *versionStatement = nullptr;
  if (sqlite3_prepare_v2(m_database, "PRAGMA user_version", -1,
                         &versionStatement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::MigrationFailed);
  const int versionStep = sqlite3_step(versionStatement);
  const int currentVersion = versionStep == SQLITE_ROW
                                 ? sqlite3_column_int(versionStatement, 0)
                                 : -1;
  sqlite3_finalize(versionStatement);
  constexpr int latestVersion = 7;
  if (currentVersion < 0 || currentVersion > latestVersion)
    return Result<void, StorageError>::failure(StorageError::MigrationFailed);

  struct Migration {
    int version;
    const char *resource;
  };
  constexpr Migration migrations[] = {
      {1, ":/openchat/001_initial.sql"},
      {2, ":/openchat/002_indexes.sql"},
      {3, ":/openchat/003_domain_alignment.sql"},
      {4, ":/openchat/004_profile_identity.sql"},
      {5, ":/openchat/005_outbox_control.sql"},
      {6, ":/openchat/006_account_identity.sql"},
      {7, ":/openchat/007_contacts.sql"},
  };

  if (!execute("BEGIN IMMEDIATE;").hasValue())
    return Result<void, StorageError>::failure(StorageError::MigrationFailed);
  for (const auto &migration : migrations) {
    if (migration.version <= currentVersion)
      continue;
    const auto sql = readMigration(QString::fromLatin1(migration.resource));
    if (!sql.hasValue() || !execute(sql.value()).hasValue()) {
      (void)execute("ROLLBACK;");
      return Result<void, StorageError>::failure(StorageError::MigrationFailed);
    }
  }
  if (!execute("COMMIT;").hasValue()) {
    (void)execute("ROLLBACK;");
    return Result<void, StorageError>::failure(StorageError::MigrationFailed);
  }
  return Result<void, StorageError>::success();
}

Result<void, StorageError> SqlCipherDatabase::execute(QByteArrayView sql) {
  if (!m_database)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  char *error = nullptr;
  const QByteArray terminated = sql.toByteArray();
  const int result = sqlite3_exec(m_database, terminated.constData(), nullptr,
                                  nullptr, &error);
  if (error)
    sqlite3_free(error);
  if (result != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  return Result<void, StorageError>::success();
}

Result<void, StorageError> SqlCipherDatabase::verifyCipherIntegrity() {
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(m_database, "PRAGMA cipher_integrity_check", -1,
                         &statement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::WrongKeyOrCorrupt);

  bool hasError = false;
  int step = SQLITE_ROW;
  while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
    const auto *text =
        reinterpret_cast<const char *>(sqlite3_column_text(statement, 0));
    if (text && QByteArrayView(text) != QByteArrayView("ok"))
      hasError = true;
  }
  sqlite3_finalize(statement);
  if (step != SQLITE_DONE || hasError)
    return Result<void, StorageError>::failure(StorageError::WrongKeyOrCorrupt);
  return Result<void, StorageError>::success();
}

Result<void, StorageError>
SqlCipherDatabase::storeVerificationMarker(QByteArrayView marker) {
  if (!m_database)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql = "INSERT INTO verification_markers(marker) VALUES(?1)";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_bind_blob(statement, 1, marker.data(),
                    static_cast<int>(marker.size()), SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (step != SQLITE_DONE)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  return Result<void, StorageError>::success();
}

Result<bool, StorageError>
SqlCipherDatabase::hasVerificationMarker(QByteArrayView marker) {
  if (!m_database)
    return Result<bool, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql =
      "SELECT 1 FROM verification_markers WHERE marker = ?1 LIMIT 1";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<bool, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_bind_blob(statement, 1, marker.data(),
                    static_cast<int>(marker.size()), SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (step != SQLITE_ROW && step != SQLITE_DONE)
    return Result<bool, StorageError>::failure(StorageError::QueryFailed);
  return Result<bool, StorageError>::success(step == SQLITE_ROW);
}

Result<void, StorageError> SqlCipherDatabase::storeDeviceIdentity(
    const ProfileId &profileId, const DeviceId &deviceId,
    QByteArrayView publicKey, const SecureBuffer &privateKey,
    const SecureBuffer &wrappingKey, qint64 createdAtMs) {
  if (!m_database || publicKey.size() != devicePublicKeySize ||
      privateKey.size() != devicePrivateKeySize ||
      wrappingKey.size() != wrappingKeySize || createdAtMs < 0)
    return Result<void, StorageError>::failure(StorageError::InvalidKey);
  const auto aad = identityAad(profileId, deviceId, publicKey);
  auto wrapped = wrapSecret(privateKey, wrappingKey, aad);
  if (!wrapped.hasValue())
    return Result<void, StorageError>::failure(wrapped.error());

  sqlite3_stmt *statement = nullptr;
  constexpr auto sql =
      "INSERT INTO local_profiles(profile_id, device_id, signing_public_key, "
      "private_nonce, private_ciphertext, private_tag, created_at_ms) "
      "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  const auto profileBytes = profileId.bytes();
  const auto deviceBytes = deviceId.bytes();
  sqlite3_bind_blob(statement, 1, profileBytes.constData(),
                    static_cast<int>(profileBytes.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 2, deviceBytes.constData(),
                    static_cast<int>(deviceBytes.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 3, publicKey.data(),
                    static_cast<int>(publicKey.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 4, wrapped.value().nonce.constData(),
                    static_cast<int>(wrapped.value().nonce.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 5, wrapped.value().ciphertext.constData(),
                    static_cast<int>(wrapped.value().ciphertext.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 6, wrapped.value().tag.constData(),
                    static_cast<int>(wrapped.value().tag.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement, 7, createdAtMs);
  const int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (step != SQLITE_DONE)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  return Result<void, StorageError>::success();
}

Result<StoredDeviceIdentity, StorageError>
SqlCipherDatabase::loadDeviceIdentity(const ProfileId &profileId,
                                      const SecureBuffer &wrappingKey) {
  if (!m_database || wrappingKey.size() != wrappingKeySize)
    return Result<StoredDeviceIdentity, StorageError>::failure(
        StorageError::InvalidKey);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql =
      "SELECT device_id, signing_public_key, private_nonce, private_ciphertext, "
      "private_tag FROM local_profiles WHERE profile_id = ?1";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<StoredDeviceIdentity, StorageError>::failure(
        StorageError::QueryFailed);
  const auto profileBytes = profileId.bytes();
  sqlite3_bind_blob(statement, 1, profileBytes.constData(),
                    static_cast<int>(profileBytes.size()),
                    SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  if (step != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return Result<StoredDeviceIdentity, StorageError>::failure(
        step == SQLITE_DONE ? StorageError::NotFound : StorageError::QueryFailed);
  }
  const auto column = [statement](int index) {
    const auto *data = static_cast<const char *>(sqlite3_column_blob(statement, index));
    const int size = sqlite3_column_bytes(statement, index);
    return data && size > 0 ? QByteArray(data, size) : QByteArray{};
  };
  const QByteArray deviceBytes = column(0);
  const QByteArray publicKey = column(1);
  const QByteArray nonce = column(2);
  const QByteArray ciphertext = column(3);
  const QByteArray tag = column(4);
  sqlite3_finalize(statement);

  const auto deviceId = DeviceId::fromBytes(deviceBytes);
  if (!deviceId || publicKey.size() != devicePublicKeySize ||
      ciphertext.size() != devicePrivateKeySize)
    return Result<StoredDeviceIdentity, StorageError>::failure(
        StorageError::AuthenticationFailed);
  const auto aad = identityAad(profileId, *deviceId, publicKey);
  auto privateKey = unwrapSecret(ciphertext, nonce, tag, wrappingKey, aad);
  if (!privateKey.hasValue())
    return Result<StoredDeviceIdentity, StorageError>::failure(privateKey.error());
  return Result<StoredDeviceIdentity, StorageError>::success(
      StoredDeviceIdentity{*deviceId, publicKey, std::move(privateKey).value()});
}

Result<QByteArray, StorageError>
SqlCipherDatabase::loadMlsState(const ProfileId &profileId) {
  if (!m_database)
    return Result<QByteArray, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql = "SELECT state_blob FROM local_mls_state WHERE profile_id = ?1";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<QByteArray, StorageError>::failure(StorageError::QueryFailed);
  const auto profileBytes = profileId.bytes();
  sqlite3_bind_blob(statement, 1, profileBytes.constData(),
                    static_cast<int>(profileBytes.size()),
                    SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  if (step == SQLITE_DONE) {
    sqlite3_finalize(statement);
    return Result<QByteArray, StorageError>::success({});
  }
  if (step != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return Result<QByteArray, StorageError>::failure(StorageError::QueryFailed);
  }
  const auto *data = static_cast<const char *>(sqlite3_column_blob(statement, 0));
  const int size = sqlite3_column_bytes(statement, 0);
  QByteArray state = data && size > 0 ? QByteArray(data, size) : QByteArray{};
  sqlite3_finalize(statement);
  if (state.size() > maximumMlsStateSize)
    return Result<QByteArray, StorageError>::failure(StorageError::AuthenticationFailed);
  return Result<QByteArray, StorageError>::success(std::move(state));
}

Result<void, StorageError>
SqlCipherDatabase::storeMlsState(const ProfileId &profileId, QByteArrayView state) {
  if (!m_database || state.size() > maximumMlsStateSize)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql =
      "INSERT INTO local_mls_state(profile_id, state_blob) VALUES(?1, ?2) "
      "ON CONFLICT(profile_id) DO UPDATE SET state_blob = excluded.state_blob";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  const auto profileBytes = profileId.bytes();
  sqlite3_bind_blob(statement, 1, profileBytes.constData(),
                    static_cast<int>(profileBytes.size()),
                    SQLITE_TRANSIENT);
  if (state.isEmpty())
    sqlite3_bind_zeroblob(statement, 2, 0);
  else
    sqlite3_bind_blob(statement, 2, state.data(), static_cast<int>(state.size()),
                      SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (step != SQLITE_DONE)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  return Result<void, StorageError>::success();
}

Result<void, StorageError>
SqlCipherDatabase::storeAccountId(const ProfileId &profileId,
                                 const AccountId &accountId) {
  if (!m_database)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql =
      "INSERT INTO local_account_identity(profile_id, account_id) "
      "VALUES(?1, ?2)";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  const auto profileBytes = profileId.bytes();
  const auto accountBytes = accountId.bytes();
  sqlite3_bind_blob(statement, 1, profileBytes.constData(),
                    static_cast<int>(profileBytes.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement, 2, accountBytes.constData(),
                    static_cast<int>(accountBytes.size()), SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (step != SQLITE_DONE)
    return Result<void, StorageError>::failure(StorageError::QueryFailed);
  return Result<void, StorageError>::success();
}

Result<AccountId, StorageError>
SqlCipherDatabase::loadAccountId(const ProfileId &profileId) {
  if (!m_database)
    return Result<AccountId, StorageError>::failure(StorageError::QueryFailed);
  sqlite3_stmt *statement = nullptr;
  constexpr auto sql =
      "SELECT account_id FROM local_account_identity WHERE profile_id = ?1";
  if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK)
    return Result<AccountId, StorageError>::failure(StorageError::QueryFailed);
  const auto profileBytes = profileId.bytes();
  sqlite3_bind_blob(statement, 1, profileBytes.constData(),
                    static_cast<int>(profileBytes.size()), SQLITE_TRANSIENT);
  const int step = sqlite3_step(statement);
  if (step != SQLITE_ROW) {
    sqlite3_finalize(statement);
    return Result<AccountId, StorageError>::failure(
        step == SQLITE_DONE ? StorageError::NotFound : StorageError::QueryFailed);
  }
  const auto *data = static_cast<const char *>(sqlite3_column_blob(statement, 0));
  const int size = sqlite3_column_bytes(statement, 0);
  const QByteArray accountBytes =
      data && size > 0 ? QByteArray(data, size) : QByteArray{};
  sqlite3_finalize(statement);
  const auto accountId = AccountId::fromBytes(accountBytes);
  if (!accountId)
    return Result<AccountId, StorageError>::failure(
        StorageError::AuthenticationFailed);
  return Result<AccountId, StorageError>::success(*accountId);
}

void SqlCipherDatabase::close() noexcept {
  if (!m_database)
    return;
  sqlite3_wal_checkpoint_v2(m_database, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                            nullptr, nullptr);
  sqlite3_close_v2(m_database);
  m_database = nullptr;
}

} // namespace OpenChat
