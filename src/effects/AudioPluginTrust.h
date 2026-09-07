#pragma once

#include "effects/AudioPluginTypes.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace OpenChat {

// The record of a user having said yes to one specific file.
//
// Recorded against the BYTES, not the path. A path is a name somebody else can
// repoint; the hash is what the user actually looked at and approved. Storing
// only the path would make consent mean "anything that ever lives here", which
// is precisely the property an attacker wants.
struct PluginConsent final {
    QString path; // Absolute, symlinks resolved.
    QString sha256;
    qint64 fileSize = 0;
    qint64 consentedMs = 0;

    [[nodiscard]] bool isValid() const noexcept
    {
        return !path.isEmpty() && sha256.size() == 64;
    }
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static PluginConsent fromJson(const QJsonObject &object);
};

// Everything that has to be true about a file before it is opened, and the one
// way to open it once it is.
//
// The threat here is not a bad plugin, it is a good plugin that somebody else
// can replace. If any directory on the way to the file is writable by another
// account, then approving the file approves whatever that account substitutes
// tomorrow -- and it lands inside the process holding the message-history key.
// So the checks are about who can WRITE the path, not about what the file
// contains, because nothing can be usefully concluded about the contents of an
// ELF object that is about to be executed.
namespace AudioPluginTrust {

// Resolves symlinks and requires an absolute path to an existing regular file
// (for VST3, to the .so inside the bundle). Everything else in this namespace
// assumes the path it is given came from here.
[[nodiscard]] QString resolve(const QString &path);

// Refuses a path that any account other than this user and root can rewrite,
// walking every ancestor directory to the root.
//
// The walk is the point. A file that is mode 0644 and owned by the user is
// still trivially replaceable if it sits in a directory that is group- or
// world-writable, because replacing a file is a property of its DIRECTORY and
// not of the file. Checking only the file itself is the mistake this exists to
// avoid. A sticky world-writable directory (/tmp) is refused too: sticky stops
// somebody deleting your file, not creating theirs beside it and pointing you
// at that.
[[nodiscard]] PluginError checkPluginPath(const QString &resolvedPath);

// SHA-256 of the whole file, streamed. Empty on failure.
[[nodiscard]] QString sha256OfFile(const QString &resolvedPath);

[[nodiscard]] qint64 fileSizeOf(const QString &resolvedPath);

// Builds the consent record for a file the user has just approved. Fails
// closed: an unreadable or untrusted path yields an invalid record rather than
// a permissive one.
[[nodiscard]] PluginConsent recordConsent(const QString &path, qint64 nowMs);

// The gate every load goes through.
//
// Re-hashes and compares. There is deliberately no "trust this always" and no
// silent re-hash on mismatch: a vendor's own auto-update is exactly the event
// this exists to surface -- in 2023 a real vendor shipped a trojanised library
// inside correctly-signed installers -- so a changed file has to be approved
// again or the control is decorative.
[[nodiscard]] PluginError verifyAgainstConsent(const QString &path,
                                               const QList<PluginConsent> &consents);

} // namespace AudioPluginTrust

} // namespace OpenChat
