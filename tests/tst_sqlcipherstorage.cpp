#include "storage/SqlCipherDatabase.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <sqlite3.h>

#include <chrono>
#include <thread>

using namespace OpenChat;

class SqlCipherStorageTest final : public QObject {
  Q_OBJECT

private slots:
  void plaintextNeverAppearsOnDisk();
  void correctKeyReopensPersistentData();
  void wrongKeyFailsClosed();
  void modifiedCiphertextFailsIntegrityCheck();
  void invalidKeyDoesNotCreateAFile();
  void anotherWritersCommitIsWaitedFor();
};

void SqlCipherStorageTest::plaintextNeverAppearsOnDisk() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
  auto key = SecureBuffer::random(32);
  auto database = SqlCipherDatabase::open(path, key);
  QVERIFY(database.hasValue());

  const QByteArray marker("OPENCHAT_SECRET_CORPUS_7fd1");
  QVERIFY(database.value().storeVerificationMarker(marker).hasValue());

  QFile walFile(path + QStringLiteral("-wal"));
  QVERIFY(walFile.open(QIODevice::ReadOnly));
  QVERIFY(!walFile.readAll().contains(marker));
  walFile.close();

  QFile databaseFile(path);
  QVERIFY(databaseFile.open(QIODevice::ReadOnly));
  const QByteArray encryptedBytes = databaseFile.readAll();
  QVERIFY(!encryptedBytes.contains(marker));
  QVERIFY(!encryptedBytes.startsWith("SQLite format 3"));
}

void SqlCipherStorageTest::wrongKeyFailsClosed() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
  auto keyA = SecureBuffer::random(32);
  auto keyB = SecureBuffer::random(32);

  auto created = SqlCipherDatabase::open(path, keyA);
  QVERIFY(created.hasValue());
  QVERIFY(created.value().storeVerificationMarker("preserve-me").hasValue());
  created.value().close();
  const qint64 originalSize = QFile(path).size();

  auto opened = SqlCipherDatabase::open(path, keyB);
  QVERIFY(!opened.hasValue());
  QCOMPARE(opened.error(), StorageError::WrongKeyOrCorrupt);
  QCOMPARE(QFile(path).size(), originalSize);
}

void SqlCipherStorageTest::correctKeyReopensPersistentData() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
  auto key = SecureBuffer::random(32);

  auto created = SqlCipherDatabase::open(path, key);
  QVERIFY(created.hasValue());
  QVERIFY(
      created.value().storeVerificationMarker("persistent-secret").hasValue());
  created.value().close();

  auto reopened = SqlCipherDatabase::open(path, key);
  QVERIFY(reopened.hasValue());
  auto present = reopened.value().hasVerificationMarker("persistent-secret");
  QVERIFY(present.hasValue());
  QVERIFY(present.value());
}

void SqlCipherStorageTest::invalidKeyDoesNotCreateAFile() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
  const auto invalidKey = SecureBuffer::fromBytes("short");

  auto opened = SqlCipherDatabase::open(path, invalidKey);
  QVERIFY(!opened.hasValue());
  QCOMPARE(opened.error(), StorageError::InvalidKey);
  QVERIFY(!QFileInfo::exists(path));
}

void SqlCipherStorageTest::modifiedCiphertextFailsIntegrityCheck() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
  auto key = SecureBuffer::random(32);

  auto created = SqlCipherDatabase::open(path, key);
  QVERIFY(created.hasValue());
  QVERIFY(created.value().storeVerificationMarker("tamper-target").hasValue());
  created.value().close();

  QFile file(path);
  QVERIFY(file.open(QIODevice::ReadWrite));
  QVERIFY(file.seek(128));
  QByteArray byte = file.read(1);
  QCOMPARE(byte.size(), 1);
  byte[0] ^= 0x40;
  QVERIFY(file.seek(128));
  QCOMPARE(file.write(byte), 1);
  file.close();

  auto reopened = SqlCipherDatabase::open(path, key);
  QVERIFY(!reopened.hasValue());
  QCOMPARE(reopened.error(), StorageError::WrongKeyOrCorrupt);
}

// A second OpenChat, or the previous one still closing, may be in the middle
// of a commit when this one opens the profile. Opening used to fail at once
// with SQLITE_BUSY, which made the app quit at startup without a word.
void SqlCipherStorageTest::anotherWritersCommitIsWaitedFor() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("profile.sqlite3"));
  auto key = SecureBuffer::random(32);
  {
    auto created = SqlCipherDatabase::open(path, key);
    QVERIFY(created.hasValue());
  }

  sqlite3 *writer = nullptr;
  QCOMPARE(sqlite3_open_v2(QFile::encodeName(path).constData(), &writer,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                           nullptr),
           SQLITE_OK);
  QCOMPARE(sqlite3_key(writer, key.view().data(), static_cast<int>(key.size())),
           SQLITE_OK);
  QCOMPARE(sqlite3_exec(writer, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr),
           SQLITE_OK);
  QCOMPARE(sqlite3_exec(writer,
                        "INSERT INTO verification_markers(marker) VALUES(x'01');",
                        nullptr, nullptr, nullptr),
           SQLITE_OK);
  std::thread commit([writer] {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    sqlite3_exec(writer, "COMMIT;", nullptr, nullptr, nullptr);
  });

  QElapsedTimer waited;
  waited.start();
  auto reopened = SqlCipherDatabase::open(path, key);
  const qint64 elapsed = waited.elapsed();
  commit.join();
  sqlite3_close_v2(writer);

  QVERIFY2(reopened.hasValue(), "opening failed while another connection was committing");
  QVERIFY2(elapsed >= 300, qPrintable(QStringLiteral("opened after %1 ms, before the writer "
                                                     "committed").arg(elapsed)));
  QVERIFY(reopened.value().hasVerificationMarker(QByteArray(1, '\x01')).value());
}

QTEST_GUILESS_MAIN(SqlCipherStorageTest)
#include "tst_sqlcipherstorage.moc"
