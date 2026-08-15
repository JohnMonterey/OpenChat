#pragma once

#include "core/Result.h"
#include "security/SecureBuffer.h"

#include <QByteArrayView>
#include <QMutex>
#include <QString>

#include <memory>
#include <utility>

struct sqlite3;

namespace OpenChat {

class SqlCipherChatRepository;
class SqlCipherOutboxRepository;
class SqlCipherSyncRepository;

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
  friend class SqlCipherChatRepository;
  friend class SqlCipherOutboxRepository;
  friend class SqlCipherSyncRepository;

  explicit SqlCipherDatabase(sqlite3 *database, QString path);

  template <typename Callback>
  auto withConnection(Callback &&callback)
      -> decltype(callback(static_cast<sqlite3 *>(nullptr))) {
    QMutexLocker locker(m_mutex.get());
    return callback(m_database);
  }

  [[nodiscard]] Result<void, StorageError> configure();
  [[nodiscard]] Result<void, StorageError> migrate();
  [[nodiscard]] Result<void, StorageError> execute(QByteArrayView sql);
  [[nodiscard]] Result<void, StorageError> verifyCipherIntegrity();

  sqlite3 *m_database = nullptr;
  QString m_path;
  std::unique_ptr<QRecursiveMutex> m_mutex;
};

} // namespace OpenChat
