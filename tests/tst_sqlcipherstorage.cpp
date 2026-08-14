#include "storage/SqlCipherDatabase.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>

using namespace OpenChat;

class SqlCipherStorageTest final : public QObject {
  Q_OBJECT

private slots:
  void plaintextNeverAppearsOnDisk();
  void correctKeyReopensPersistentData();
  void wrongKeyFailsClosed();
  void modifiedCiphertextFailsIntegrityCheck();
  void invalidKeyDoesNotCreateAFile();
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

QTEST_GUILESS_MAIN(SqlCipherStorageTest)
#include "tst_sqlcipherstorage.moc"
