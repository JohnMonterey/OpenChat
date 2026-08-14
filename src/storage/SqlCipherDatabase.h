#pragma once

#include "core/Result.h"
#include "security/SecureBuffer.h"

#include <QByteArrayView>
#include <QString>

struct sqlite3;

namespace OpenChat {

enum class StorageError {
  InvalidKey,
  CannotOpen,
  WrongKeyOrCorrupt,
  MigrationFailed,
  QueryFailed
};

class SqlCipherDatabase final {
public:
  ~SqlCipherDatabase();

  SqlCipherDatabase(const SqlCipherDatabase &) = delete;
  SqlCipherDatabase &operator=(const SqlCipherDatabase &) = delete;
  SqlCipherDatabase(SqlCipherDatabase &&other) noexcept;
  SqlCipherDatabase &operator=(SqlCipherDatabase &&other) noexcept;

  [[nodiscard]] static Result<SqlCipherDatabase, StorageError>
  open(const QString &path, const SecureBuffer &key);

  [[nodiscard]] Result<void, StorageError>
  storeVerificationMarker(QByteArrayView marker);
  [[nodiscard]] Result<bool, StorageError>
  hasVerificationMarker(QByteArrayView marker);
  void close() noexcept;

private:
  explicit SqlCipherDatabase(sqlite3 *database, QString path);

  [[nodiscard]] Result<void, StorageError> configure();
  [[nodiscard]] Result<void, StorageError> migrate();
  [[nodiscard]] Result<void, StorageError> execute(QByteArrayView sql);
  [[nodiscard]] Result<void, StorageError> verifyCipherIntegrity();

  sqlite3 *m_database = nullptr;
  QString m_path;
};

} // namespace OpenChat
