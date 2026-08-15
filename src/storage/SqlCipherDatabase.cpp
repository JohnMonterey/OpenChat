#include "storage/SqlCipherDatabase.h"

#include <QFile>
#include <QFileInfo>

#include <sqlite3.h>

#include <utility>

namespace OpenChat {
namespace {

constexpr qsizetype profileKeySize = 32;

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
  constexpr int latestVersion = 3;
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

void SqlCipherDatabase::close() noexcept {
  if (!m_database)
    return;
  sqlite3_wal_checkpoint_v2(m_database, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                            nullptr, nullptr);
  sqlite3_close_v2(m_database);
  m_database = nullptr;
}

} // namespace OpenChat
