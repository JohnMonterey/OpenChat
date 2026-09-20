#pragma once

#include "app/ProfileSession.h"

#include <QString>
#include <QStringList>

namespace OpenChat {

class KeyVault;

// Account layouts.
//
// Layout 1 is the original account: a handle and a device key, with no password.
// Such an account cannot be signed in to again, so a profile that still uses it
// is unusable under the username + password model and is erased on startup; the
// user then goes through onboarding again.
//
// Layout 2 is the username + password account. A profile is stamped with it only
// once its account is registered (or logged in to) on the relay WITH a password,
// by writing a small marker file into the profile directory.
//
// The marker deliberately lives outside the encrypted database. Telling the two
// layouts apart must not depend on being able to unlock the profile: the erase
// below destroys the keychain entries first, so an interrupted erase leaves a
// profile that can never be unlocked again -- and it must still be recognised as
// old and finished off on the next start. It records no secret, and removing it
// can only cause the profile to be erased, which anyone able to remove it could
// already do directly.
inline constexpr int currentAccountLayout = 2;

[[nodiscard]] bool profileHasCurrentAccountLayout(const ProfilePaths &paths);
[[nodiscard]] bool markProfileAccountLayoutCurrent(const ProfilePaths &paths);

// Everything OpenChat persists on this machine apart from its preferences
// (appearance, microphone and the like are not account data and are kept).
struct LocalDataLocations final {
  QString profilesRoot; // <appData>/profiles
  QString appDataDir;   // profiles, logs and any other application data
  QString cacheDir;     // QML and shader caches
  QStringList tempDirs; // OpenChat-owned directories under the system temp dir

  // The real locations for this installation, from QStandardPaths.
  [[nodiscard]] static LocalDataLocations standard(const QString &profilesRoot);
};

struct LocalDataResetReport final {
  int profilesErased = 0;
  // Paths that could not be removed and keychain entries that could not be
  // deleted. Empty means the erase is complete.
  QStringList failures;

  [[nodiscard]] bool complete() const { return failures.isEmpty(); }
};

// True when `profilesRoot` holds at least one profile directory that is not
// stamped with the current account layout.
[[nodiscard]] bool hasOutdatedProfiles(const QString &profilesRoot);

// Erases every outdated profile and all cached / temporary application data.
//
// For each outdated profile the keychain entries go first. The profile database
// is encrypted under a key that exists only in the keychain, so once that entry
// is gone whatever remains of the file on disk is permanently unreadable -- this
// is what makes the erase a true one even on flash storage and journaling
// filesystems, where overwriting a file in place does not reliably destroy its
// old blocks. The files are then overwritten and unlinked as well, which does
// cover the unencrypted ones (logs) on storage where an overwrite is honoured.
//
// A profile already stamped with the current layout is never touched. The rest
// of the application data directory is cleared only when no such profile
// remains; caches and temp directories are always cleared (they are rebuilt on
// demand). Safe to call repeatedly: a second run finishes what a first could not.
LocalDataResetReport eraseOutdatedLocalData(KeyVault &vault,
                                            const LocalDataLocations &locations);

} // namespace OpenChat
