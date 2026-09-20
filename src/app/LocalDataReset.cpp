#include "app/LocalDataReset.h"

#include "security/KeyVault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace OpenChat {

namespace {

// Per-file ceiling on the zeroing pass, so an unexpectedly huge stray file
// cannot stall startup. Profile databases and logs are far below it.
constexpr qint64 maxOverwriteBytes = 64LL * 1024 * 1024;

constexpr QDir::Filters everyEntry =
    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

QString markerPath(const ProfilePaths &paths)
{
  return QDir(paths.profileDirectory).filePath(QStringLiteral("account-layout"));
}

QString markerPathForDirectory(const QString &profileDirectory)
{
  return QDir(profileDirectory).filePath(QStringLiteral("account-layout"));
}

bool markerIsCurrent(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  bool ok = false;
  const int layout = file.read(16).trimmed().toInt(&ok);
  // A marker written by a newer build is still not an outdated profile.
  return ok && layout >= currentAccountLayout;
}

// An erase target must be an absolute path at least three levels deep (for
// example /home/user/x). An empty or relative location -- which QStandardPaths
// can return on a misconfigured system -- would otherwise resolve against the
// working directory, and a shallow one could name a home directory or a drive.
bool isSafeEraseTarget(const QString &path)
{
  if (path.isEmpty() || !QDir::isAbsolutePath(path))
    return false;
  const QString clean = QDir::cleanPath(path);
  if (clean == QDir::cleanPath(QDir::homePath()) || clean == QDir::rootPath())
    return false;
  return clean.count(QLatin1Char('/')) >= 3;
}

void flushToDisk(QFile &file)
{
  file.flush();
#ifdef Q_OS_WIN
  _commit(file.handle());
#else
  ::fsync(file.handle());
#endif
}

// Zero-fills (when asked) and unlinks one file. A symlink is unlinked, never
// followed: the erase must not reach outside the directory it was given.
bool removeFile(const QString &path, bool overwrite)
{
  const QFileInfo info(path);
  if (overwrite && !info.isSymLink()) {
    QFile file(path);
    file.setPermissions(file.permissions() | QFileDevice::WriteOwner);
    if (file.open(QIODevice::ReadWrite)) {
      const QByteArray zeros(64 * 1024, '\0');
      qint64 remaining = std::min(file.size(), maxOverwriteBytes);
      while (remaining > 0) {
        const qint64 written =
            file.write(zeros.constData(), std::min<qint64>(remaining, zeros.size()));
        if (written <= 0)
          break;
        remaining -= written;
      }
      flushToDisk(file);
      file.close();
    }
  }
  return QFile::remove(path) || !QFileInfo::exists(path);
}

// Removes a directory tree bottom-up without following symlinks, recording every
// path that survives.
void removeTree(const QString &directory, bool overwrite, QStringList &failures)
{
  const QFileInfoList entries = QDir(directory).entryInfoList(everyEntry);
  for (const QFileInfo &entry : entries) {
    const QString path = entry.absoluteFilePath();
    if (entry.isDir() && !entry.isSymLink()) {
      removeTree(path, overwrite, failures);
    } else if (!removeFile(path, overwrite)) {
      failures.append(path);
    }
  }
  if (!QDir().rmdir(directory) && QFileInfo::exists(directory))
    failures.append(directory);
}

void removeEntry(const QFileInfo &entry, bool overwrite, QStringList &failures)
{
  if (entry.isDir() && !entry.isSymLink())
    removeTree(entry.absoluteFilePath(), overwrite, failures);
  else if (!removeFile(entry.absoluteFilePath(), overwrite))
    failures.append(entry.absoluteFilePath());
}

void deleteVaultEntries(KeyVault &vault, const QString &directoryName, QStringList &failures)
{
  const std::optional<ProfileId> id =
      ProfileId::fromBytes(QByteArray::fromHex(directoryName.toLatin1()));
  if (!id || id->toHex() != directoryName)
    return; // not a profile id, so it never had keychain entries

  // A missing entry is the goal, not a failure (it is also what a second pass
  // over an interrupted erase finds).
  const auto wrapping = vault.deleteDeviceWrappingKey(*id);
  if (!wrapping.hasValue() && wrapping.error() != KeyVaultError::NotFound)
    failures.append(QStringLiteral("keychain:device-wrapping-key/") + directoryName);
  const auto profile = vault.deleteProfileKey(*id);
  if (!profile.hasValue() && profile.error() != KeyVaultError::NotFound)
    failures.append(QStringLiteral("keychain:profile-key/") + directoryName);
}

} // namespace

bool profileHasCurrentAccountLayout(const ProfilePaths &paths)
{
  return markerIsCurrent(markerPath(paths));
}

bool markProfileAccountLayoutCurrent(const ProfilePaths &paths)
{
  QSaveFile file(markerPath(paths));
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(QByteArray::number(currentAccountLayout) + '\n');
  return file.commit();
}

LocalDataLocations LocalDataLocations::standard(const QString &profilesRoot)
{
  LocalDataLocations locations;
  locations.profilesRoot = profilesRoot;
  locations.appDataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  locations.cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  const QString temp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  if (!temp.isEmpty())
    locations.tempDirs.append(QDir(temp).filePath(QStringLiteral("OpenChat-notifications")));
  return locations;
}

bool hasOutdatedProfiles(const QString &profilesRoot)
{
  const QFileInfoList entries =
      QDir(profilesRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
  return std::any_of(entries.cbegin(), entries.cend(), [](const QFileInfo &entry) {
    return !markerIsCurrent(markerPathForDirectory(entry.absoluteFilePath()));
  });
}

LocalDataResetReport eraseOutdatedLocalData(KeyVault &vault,
                                            const LocalDataLocations &locations)
{
  LocalDataResetReport report;
  if (!isSafeEraseTarget(locations.profilesRoot)) {
    report.failures.append(locations.profilesRoot);
    return report;
  }

  bool keptCurrentProfile = false;
  const QFileInfoList profileEntries = QDir(locations.profilesRoot).entryInfoList(everyEntry);
  for (const QFileInfo &entry : profileEntries) {
    const bool isDirectory = entry.isDir() && !entry.isSymLink();
    if (isDirectory && markerIsCurrent(markerPathForDirectory(entry.absoluteFilePath()))) {
      keptCurrentProfile = true;
      continue;
    }
    if (isDirectory) {
      // Keys first: from here on the database is unreadable whatever else fails.
      deleteVaultEntries(vault, entry.fileName(), report.failures);
      ++report.profilesErased;
    }
    removeEntry(entry, /*overwrite=*/true, report.failures);
  }

  // Logs and anything else beside the profiles, but only when this really is the
  // profiles directory of that data directory, and nothing current still lives
  // in it.
  const QString appData = QDir::cleanPath(locations.appDataDir);
  const bool profilesInsideAppData = isSafeEraseTarget(appData)
      && QDir::cleanPath(locations.profilesRoot).startsWith(appData + QLatin1Char('/'));
  if (!keptCurrentProfile && profilesInsideAppData) {
    const QString profilesName =
        QDir(appData).relativeFilePath(locations.profilesRoot).section(QLatin1Char('/'), 0, 0);
    const QFileInfoList dataEntries = QDir(appData).entryInfoList(everyEntry);
    for (const QFileInfo &entry : dataEntries) {
      if (entry.fileName() != profilesName)
        removeEntry(entry, /*overwrite=*/true, report.failures);
    }
  }

  // Caches and temp files hold nothing secret and are rebuilt on demand, so they
  // are simply removed.
  QStringList disposable = locations.tempDirs;
  disposable.prepend(locations.cacheDir);
  for (const QString &directory : disposable) {
    if (!isSafeEraseTarget(directory) || !QFileInfo::exists(directory))
      continue;
    const QFileInfoList entries = QDir(directory).entryInfoList(everyEntry);
    for (const QFileInfo &entry : entries)
      removeEntry(entry, /*overwrite=*/false, report.failures);
  }
  return report;
}

} // namespace OpenChat
