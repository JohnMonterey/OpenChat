#include <QtTest>

#include "app/LocalDataReset.h"
#include "app/ProfileSession.h"
#include "security/KeyVault.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <map>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace OpenChat;

namespace {

// A keychain stand-in that holds keys per profile, so a test can tell exactly
// which profiles' entries an erase destroyed.
class MultiProfileVault final : public KeyVault {
public:
  KeyVaultAvailability availability() const override {
    return KeyVaultAvailability::Available;
  }

  Result<SecureBuffer, KeyVaultError> readProfileKey(const ProfileId &id) override {
    return read(m_profileKeys, id);
  }
  Result<SecureBuffer, KeyVaultError> createProfileKey(const ProfileId &id) override {
    return create(m_profileKeys, id);
  }
  Result<void, KeyVaultError> deleteProfileKey(const ProfileId &id) override {
    return remove(m_profileKeys, id);
  }
  Result<SecureBuffer, KeyVaultError> readDeviceWrappingKey(const ProfileId &id) override {
    return read(m_wrappingKeys, id);
  }
  Result<SecureBuffer, KeyVaultError> createDeviceWrappingKey(const ProfileId &id) override {
    return create(m_wrappingKeys, id);
  }
  Result<void, KeyVaultError> deleteDeviceWrappingKey(const ProfileId &id) override {
    return remove(m_wrappingKeys, id);
  }

  [[nodiscard]] bool holdsAnythingFor(const ProfileId &id) const {
    return m_profileKeys.count(id.toHex()) > 0 || m_wrappingKeys.count(id.toHex()) > 0;
  }

  bool failDeletes = false;

private:
  using Keys = std::map<QString, SecureBuffer>;

  static Result<SecureBuffer, KeyVaultError> read(const Keys &keys, const ProfileId &id) {
    const auto found = keys.find(id.toHex());
    if (found == keys.end())
      return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::NotFound);
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(found->second.view()));
  }
  static Result<SecureBuffer, KeyVaultError> create(Keys &keys, const ProfileId &id) {
    if (keys.count(id.toHex()) > 0)
      return Result<SecureBuffer, KeyVaultError>::failure(KeyVaultError::AlreadyExists);
    auto inserted = keys.emplace(id.toHex(), SecureBuffer::random(32));
    return Result<SecureBuffer, KeyVaultError>::success(
        SecureBuffer::fromBytes(inserted.first->second.view()));
  }
  Result<void, KeyVaultError> remove(Keys &keys, const ProfileId &id) const {
    if (failDeletes)
      return Result<void, KeyVaultError>::failure(KeyVaultError::StorageFailure);
    if (keys.erase(id.toHex()) == 0)
      return Result<void, KeyVaultError>::failure(KeyVaultError::NotFound);
    return Result<void, KeyVaultError>::success();
  }

  Keys m_profileKeys;
  Keys m_wrappingKeys;
};

void writeFile(const QString &path, const QByteArray &content)
{
  QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write(content), content.size());
}

QStringList everythingUnder(const QString &directory)
{
  QStringList found;
  QDirIterator it(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                  QDirIterator::Subdirectories);
  while (it.hasNext())
    found.append(QDir(directory).relativeFilePath(it.next()));
  found.sort();
  return found;
}

// An application data tree shaped like a real installation's.
struct Installation {
  QTemporaryDir root;
  MultiProfileVault vault;
  LocalDataLocations locations;

  Installation()
  {
    const QDir base(root.path());
    locations.appDataDir = base.filePath(QStringLiteral("share/OpenChat"));
    locations.profilesRoot = base.filePath(QStringLiteral("share/OpenChat/profiles"));
    locations.cacheDir = base.filePath(QStringLiteral("cache/OpenChat"));
    locations.tempDirs = {base.filePath(QStringLiteral("tmp/OpenChat-notifications"))};
    QDir().mkpath(locations.profilesRoot);
  }

  // A real, unlockable profile (encrypted database + two keychain entries).
  ProfileId addProfile(bool currentLayout)
  {
    const ProfileId id = ProfileId::generate();
    const ProfilePaths paths = ProfilePaths::forProfile(locations.profilesRoot, id);
    // Not Q_ASSERT: these have side effects and must run in release builds too.
    auto created = ProfileSession::create(id, vault, paths);
    if (!created.hasValue())
      qFatal("could not create a test profile");
    created.value()->lock();
    if (currentLayout && !markProfileAccountLayoutCurrent(paths))
      qFatal("could not stamp a test profile");
    return id;
  }

  [[nodiscard]] QString profileDir(const ProfileId &id) const
  {
    return ProfilePaths::forProfile(locations.profilesRoot, id).profileDirectory;
  }
};

} // namespace

class LocalDataResetTest final : public QObject {
  Q_OBJECT

private slots:
  void newProfileIsOutdatedUntilStamped()
  {
    Installation install;
    QVERIFY(!hasOutdatedProfiles(install.locations.profilesRoot)); // nothing at all yet

    const ProfileId id = install.addProfile(/*currentLayout=*/false);
    const ProfilePaths paths = ProfilePaths::forProfile(install.locations.profilesRoot, id);
    // Exactly what a profile from before usernames and passwords looks like --
    // and what a sign-up that crashed before finishing leaves behind.
    QVERIFY(!profileHasCurrentAccountLayout(paths));
    QVERIFY(hasOutdatedProfiles(install.locations.profilesRoot));

    QVERIFY(markProfileAccountLayoutCurrent(paths));
    QVERIFY(profileHasCurrentAccountLayout(paths));
    QVERIFY(!hasOutdatedProfiles(install.locations.profilesRoot));
  }

  void markerContentDecidesNotItsPresence()
  {
    Installation install;
    const ProfileId id = install.addProfile(false);
    const ProfilePaths paths = ProfilePaths::forProfile(install.locations.profilesRoot, id);
    const QString marker = QDir(paths.profileDirectory).filePath(QStringLiteral("account-layout"));

    for (const QByteArray &stale : {QByteArray("1\n"), QByteArray(""), QByteArray("two"),
                                    QByteArray("-2"), QByteArray("0")}) {
      writeFile(marker, stale);
      QVERIFY2(!profileHasCurrentAccountLayout(paths), stale.constData());
    }
    // A stamp from a newer build is still not an outdated profile: never erase
    // on a downgrade.
    writeFile(marker, "3\n");
    QVERIFY(profileHasCurrentAccountLayout(paths));
  }

  void eraseRemovesOutdatedProfileKeysCachesAndLogs()
  {
    Installation install;
    const ProfileId legacy = install.addProfile(false);
    const QString database =
        ProfilePaths::forProfile(install.locations.profilesRoot, legacy).database;
    QVERIFY(QFileInfo(database).isFile());
    QVERIFY(install.vault.holdsAnythingFor(legacy));

    const QDir appData(install.locations.appDataDir);
    writeFile(appData.filePath(QStringLiteral("openchat.log")), "handle=alice connected\n");
    writeFile(appData.filePath(QStringLiteral("openchat.log.1")), "older log\n");
    writeFile(appData.filePath(QStringLiteral("stray/nested/thing.json")), "{}");
    writeFile(QDir(install.locations.cacheDir).filePath(QStringLiteral("qmlcache/a.qmlc")), "x");
    writeFile(QDir(install.locations.tempDirs.first()).filePath(QStringLiteral("avatar.png")), "x");

    const LocalDataResetReport report =
        eraseOutdatedLocalData(install.vault, install.locations);

    QVERIFY2(report.complete(), qPrintable(report.failures.join(QLatin1Char(','))));
    QCOMPARE(report.profilesErased, 1);
    // The keychain entries are gone, so whatever survives of the encrypted
    // database on the physical disk can never be read again.
    QVERIFY(!install.vault.holdsAnythingFor(legacy));
    // And nothing survives in the filesystem either.
    QCOMPARE(everythingUnder(install.locations.profilesRoot), QStringList());
    QCOMPARE(everythingUnder(install.locations.appDataDir),
             QStringList{QStringLiteral("profiles")});
    QCOMPARE(everythingUnder(install.locations.cacheDir), QStringList());
    QCOMPARE(everythingUnder(install.locations.tempDirs.first()), QStringList());
    QVERIFY(!hasOutdatedProfiles(install.locations.profilesRoot));
  }

  void filesAreZeroedBeforeTheyAreUnlinked()
  {
    // Hold a second hard link to the log: after the erase it still names the
    // same disk blocks the unlinked file had, so it shows what was left in them.
    Installation install;
    install.addProfile(false);
    const QString log = QDir(install.locations.appDataDir).filePath(QStringLiteral("openchat.log"));
    const QByteArray secret("contact request from @bob accepted\n");
    writeFile(log, secret);
    const QString witness = QDir(install.root.path()).filePath(QStringLiteral("witness"));
    if (!QFile::link(log, witness) || QFileInfo(witness).isSymLink()) {
      QFile::remove(witness);
#ifdef Q_OS_UNIX
      QVERIFY(::link(QFile::encodeName(log).constData(),
                     QFile::encodeName(witness).constData()) == 0);
#else
      QSKIP("hard links are not available to observe the overwrite");
#endif
    }

    QVERIFY(eraseOutdatedLocalData(install.vault, install.locations).complete());

    QVERIFY(!QFileInfo::exists(log));
    QFile remains(witness);
    QVERIFY(remains.open(QIODevice::ReadOnly));
    const QByteArray left = remains.readAll();
    QCOMPARE(left.size(), secret.size());
    QCOMPARE(left, QByteArray(secret.size(), '\0'));
  }

  void currentProfileAndItsDataAreNeverTouched()
  {
    Installation install;
    const ProfileId legacy = install.addProfile(false);
    const ProfileId current = install.addProfile(true);
    const QString log = QDir(install.locations.appDataDir).filePath(QStringLiteral("openchat.log"));
    writeFile(log, "current era log\n");
    writeFile(QDir(install.locations.cacheDir).filePath(QStringLiteral("qmlcache/a.qmlc")), "x");
    const QStringList currentBefore = everythingUnder(install.profileDir(current));

    const LocalDataResetReport report =
        eraseOutdatedLocalData(install.vault, install.locations);

    QVERIFY(report.complete());
    QCOMPARE(report.profilesErased, 1);
    QVERIFY(!QFileInfo::exists(install.profileDir(legacy)));
    QVERIFY(!install.vault.holdsAnythingFor(legacy));

    // The account that is in use keeps its files, its keys -- it still unlocks --
    // and the log written during its lifetime.
    QCOMPARE(everythingUnder(install.profileDir(current)), currentBefore);
    QVERIFY(install.vault.holdsAnythingFor(current));
    auto unlocked = ProfileSession::unlock(
        current, install.vault,
        ProfilePaths::forProfile(install.locations.profilesRoot, current));
    QVERIFY(unlocked.hasValue());
    QVERIFY(QFileInfo::exists(log));
    // Caches are disposable either way.
    QCOMPARE(everythingUnder(install.locations.cacheDir), QStringList());
  }

  void interruptedEraseIsFinishedOnTheNextRun()
  {
    // The worst interruption: the keychain entries are already destroyed (so the
    // profile can never be unlocked to inspect it) but its files are still there.
    Installation install;
    const ProfileId legacy = install.addProfile(false);
    QVERIFY(install.vault.deleteProfileKey(legacy).hasValue());
    QVERIFY(install.vault.deleteDeviceWrappingKey(legacy).hasValue());
    QVERIFY(!ProfileSession::unlock(
                 legacy, install.vault,
                 ProfilePaths::forProfile(install.locations.profilesRoot, legacy))
                 .hasValue());

    // It is still recognised as outdated, and the missing entries are not errors.
    QVERIFY(hasOutdatedProfiles(install.locations.profilesRoot));
    const LocalDataResetReport report =
        eraseOutdatedLocalData(install.vault, install.locations);
    QVERIFY2(report.complete(), qPrintable(report.failures.join(QLatin1Char(','))));
    QCOMPARE(everythingUnder(install.locations.profilesRoot), QStringList());
  }

  void keychainFailureIsReportedButDataIsStillErased()
  {
    Installation install;
    const ProfileId legacy = install.addProfile(false);
    install.vault.failDeletes = true;

    const LocalDataResetReport report =
        eraseOutdatedLocalData(install.vault, install.locations);

    QVERIFY(!report.complete());
    QCOMPARE(report.failures.size(), 2);
    QVERIFY(report.failures.first().startsWith(QStringLiteral("keychain:")));
    QVERIFY(!QFileInfo::exists(install.profileDir(legacy)));
  }

  void symlinksAreUnlinkedNeverFollowed()
  {
#ifndef Q_OS_UNIX
    QSKIP("symbolic link semantics differ on this platform");
#else
    Installation install;
    const ProfileId legacy = install.addProfile(false);

    // Something valuable OUTSIDE the application's directories, reachable through
    // links planted inside the profile that is about to be erased.
    const QString outsideDir = QDir(install.root.path()).filePath(QStringLiteral("documents"));
    const QString outsideFile = QDir(outsideDir).filePath(QStringLiteral("thesis.txt"));
    writeFile(outsideFile, "years of work");
    QVERIFY(QFile::link(outsideDir, QDir(install.profileDir(legacy)).filePath(QStringLiteral("dir-link"))));
    QVERIFY(QFile::link(outsideFile, QDir(install.profileDir(legacy)).filePath(QStringLiteral("file-link"))));

    QVERIFY(eraseOutdatedLocalData(install.vault, install.locations).complete());

    QVERIFY(!QFileInfo::exists(install.profileDir(legacy)));
    QFile survivor(outsideFile);
    QVERIFY(survivor.open(QIODevice::ReadOnly));
    QCOMPARE(survivor.readAll(), QByteArray("years of work"));
#endif
  }

  void unsafeLocationsAreRefused_data()
  {
    QTest::addColumn<QString>("profilesRoot");

    QTest::newRow("empty") << QString();
    QTest::newRow("relative") << QStringLiteral("profiles");
    QTest::newRow("filesystem root") << QDir::rootPath();
    QTest::newRow("home directory") << QDir::homePath();
    QTest::newRow("too shallow") << QStringLiteral("/tmp");
  }

  void unsafeLocationsAreRefused()
  {
    QFETCH(QString, profilesRoot);

    // A location that QStandardPaths failed to resolve must never turn into an
    // erase of the working directory, a home directory or a drive.
    MultiProfileVault vault;
    LocalDataLocations locations;
    locations.profilesRoot = profilesRoot;
    locations.appDataDir = profilesRoot;
    locations.cacheDir = profilesRoot;
    locations.tempDirs = {profilesRoot};

    const QStringList homeBefore = QDir::home().entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    const LocalDataResetReport report = eraseOutdatedLocalData(vault, locations);

    QVERIFY(!report.complete());
    QCOMPARE(report.profilesErased, 0);
    QCOMPARE(QDir::home().entryList(QDir::AllEntries | QDir::NoDotAndDotDot), homeBefore);
  }

  void dataBesideProfilesIsLeftAloneUnlessItIsTheAppDataDir()
  {
    // A profiles root that is NOT inside the configured data directory (a custom
    // layout): only the profiles themselves may be erased.
    Installation install;
    install.addProfile(false);
    const QString elsewhere = QDir(install.root.path()).filePath(QStringLiteral("other/data"));
    writeFile(QDir(elsewhere).filePath(QStringLiteral("keep.me")), "unrelated");
    install.locations.appDataDir = elsewhere;

    QVERIFY(eraseOutdatedLocalData(install.vault, install.locations).complete());

    QCOMPARE(everythingUnder(install.locations.profilesRoot), QStringList());
    QVERIFY(QFileInfo::exists(QDir(elsewhere).filePath(QStringLiteral("keep.me"))));
  }
};

QTEST_GUILESS_MAIN(LocalDataResetTest)

#include "tst_localdatareset.moc"
