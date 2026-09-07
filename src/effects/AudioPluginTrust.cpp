#include "effects/AudioPluginTrust.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <sys/stat.h>
#include <unistd.h>

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("AudioPluginTrust", text);
}

// True when an account other than the owner (when the owner is this user or
// root) can write this inode.
//
// Root is accepted because a root-writable path is one the packaging system
// owns; if root is hostile the plugin loader is not where that is decided.
bool isWritableByOthers(const struct stat &info)
{
    const uid_t self = ::geteuid();
    if (info.st_uid != self && info.st_uid != 0)
        return true; // Owned by a third party, who can rewrite it at will.
    // S_IWGRP even for a group we are in: membership is not exclusivity.
    return (info.st_mode & (S_IWGRP | S_IWOTH)) != 0;
}

} // namespace

QJsonObject PluginConsent::toJson() const
{
    return QJsonObject{
        {QStringLiteral("path"), path},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("size"), static_cast<double>(fileSize)},
        {QStringLiteral("consentedMs"), static_cast<double>(consentedMs)},
    };
}

PluginConsent PluginConsent::fromJson(const QJsonObject &object)
{
    PluginConsent consent;
    consent.path = object.value(QStringLiteral("path")).toString();
    consent.sha256 = object.value(QStringLiteral("sha256")).toString();
    consent.fileSize = static_cast<qint64>(object.value(QStringLiteral("size")).toDouble());
    consent.consentedMs =
        static_cast<qint64>(object.value(QStringLiteral("consentedMs")).toDouble());
    return consent;
}

namespace AudioPluginTrust {

QString resolve(const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isFile())
        return {};
    return canonical;
}

PluginError checkPluginPath(const QString &resolvedPath)
{
    if (resolvedPath.isEmpty() || !QDir::isAbsolutePath(resolvedPath)) {
        return makePluginError(PluginErrorCode::UntrustedPath,
                               tr("The plugin path could not be resolved."));
    }

    struct stat info {};
    if (::lstat(resolvedPath.toLocal8Bit().constData(), &info) != 0) {
        return makePluginError(PluginErrorCode::UntrustedPath,
                               tr("The plugin file could not be read."));
    }
    // resolve() already followed every link, so anything still a link here
    // appeared between the two calls.
    if (!S_ISREG(info.st_mode)) {
        return makePluginError(PluginErrorCode::UntrustedPath,
                               tr("The plugin path is not a regular file."));
    }
    if (isWritableByOthers(info)) {
        return makePluginError(
            PluginErrorCode::UntrustedPath,
            tr("The plugin file can be modified by other users on this computer."));
    }

    // Walk every ancestor to the root. Replacing a file is a property of its
    // directory, so a perfectly-permissioned plugin inside a world-writable
    // directory is not safe to approve.
    QDir directory = QFileInfo(resolvedPath).absoluteDir();
    forever {
        const QString directoryPath = directory.absolutePath();
        struct stat directoryInfo {};
        if (::lstat(directoryPath.toLocal8Bit().constData(), &directoryInfo) != 0) {
            return makePluginError(PluginErrorCode::UntrustedPath,
                                   tr("A directory containing the plugin could not be read."));
        }
        if (isWritableByOthers(directoryInfo)) {
            return makePluginError(
                PluginErrorCode::UntrustedPath,
                tr("The plugin is in a directory that other users on this computer can "
                   "write to, so its contents could be replaced."));
        }
        if (directory.isRoot())
            break;
        if (!directory.cdUp())
            break;
    }

    return {};
}

QString sha256OfFile(const QString &resolvedPath)
{
    QFile file(resolvedPath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

qint64 fileSizeOf(const QString &resolvedPath)
{
    const QFileInfo info(resolvedPath);
    return info.exists() ? info.size() : -1;
}

PluginConsent recordConsent(const QString &path, qint64 nowMs)
{
    const QString resolved = resolve(path);
    if (resolved.isEmpty())
        return {};
    if (checkPluginPath(resolved))
        return {};

    PluginConsent consent;
    consent.path = resolved;
    consent.sha256 = sha256OfFile(resolved);
    consent.fileSize = fileSizeOf(resolved);
    consent.consentedMs = nowMs;
    return consent.isValid() ? consent : PluginConsent{};
}

PluginError verifyAgainstConsent(const QString &path, const QList<PluginConsent> &consents)
{
    const QString resolved = resolve(path);
    if (resolved.isEmpty()) {
        return makePluginError(PluginErrorCode::UntrustedPath,
                               tr("The plugin file is no longer there."));
    }
    if (const PluginError pathError = checkPluginPath(resolved))
        return pathError;

    const PluginConsent *match = nullptr;
    for (const PluginConsent &consent : consents) {
        if (consent.path == resolved) {
            match = &consent;
            break;
        }
    }
    if (!match) {
        return makePluginError(PluginErrorCode::NotApproved,
                               tr("This plugin has not been approved for use in calls."));
    }

    // Size first: cheap, and it turns the common case of a wholesale
    // replacement into an answer without reading the file.
    if (fileSizeOf(resolved) != match->fileSize || sha256OfFile(resolved) != match->sha256) {
        return makePluginError(
            PluginErrorCode::HashMismatch,
            tr("This plugin file has changed since it was approved. Approve it again to "
               "confirm you trust the new version."));
    }
    return {};
}

} // namespace AudioPluginTrust

} // namespace OpenChat
