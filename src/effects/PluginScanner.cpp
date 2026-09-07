#include "effects/PluginScanner.h"

#include "effects/AudioPluginTrust.h"
#include "effects/ClapModule.h"
#include "effects/Vst3Module.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcessEnvironment>

namespace OpenChat {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("PluginScanner", text);
}

// Bounded so a symlink loop or a pathological tree cannot turn a scan into a
// walk of the whole filesystem.
constexpr int maxScanDepth = 6;
constexpr int maxBundles = 4096;

} // namespace

namespace PluginScanner {

QStringList searchPaths(AudioPluginFormat format)
{
    const QString home = QDir::homePath();
    QStringList paths;

    if (format == AudioPluginFormat::Clap) {
        paths << home + QLatin1String("/.clap") << QStringLiteral("/usr/lib/clap")
              << QStringLiteral("/usr/local/lib/clap");
        const QString environmentPath =
            QProcessEnvironment::systemEnvironment().value(QStringLiteral("CLAP_PATH"));
        if (!environmentPath.isEmpty())
            paths << environmentPath.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    } else {
        // No VST3_PATH: the specification does not define one.
        paths << home + QLatin1String("/.vst3") << QStringLiteral("/usr/lib/vst3")
              << QStringLiteral("/usr/lib64/vst3") << QStringLiteral("/usr/local/lib/vst3")
              << QStringLiteral("/usr/local/lib64/vst3");
    }

    QStringList existing;
    for (const QString &path : std::as_const(paths)) {
        if (!existing.contains(path) && QFileInfo(path).isDir())
            existing.append(path);
    }
    return existing;
}

QStringList findBundles(AudioPluginFormat format, const QStringList &roots)
{
    const QStringList searchRoots = roots.isEmpty() ? searchPaths(format) : roots;
    const QString extension = audioPluginFormatExtension(format);
    // A VST3 bundle is a directory and a CLAP is a file, so the two formats
    // need different entry filters from the same walk.
    const bool wantDirectories = format == AudioPluginFormat::Vst3;

    QStringList found;
    for (const QString &root : searchRoots) {
        const QDir rootDir(root);
        if (!rootDir.exists())
            continue;
        const int rootDepth = rootDir.absolutePath().count(QLatin1Char('/'));

        QDirIterator it(rootDir.absolutePath(),
                        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && found.size() < maxBundles) {
            const QString path = it.next();
            if (path.count(QLatin1Char('/')) - rootDepth > maxScanDepth)
                continue;
            if (!path.endsWith(extension, Qt::CaseInsensitive))
                continue;
            const QFileInfo info(path);
            // A VST3 shipped as a bare .so is accepted too, which is why the
            // directory test is not exclusive.
            if (wantDirectories ? !(info.isDir() || info.isFile()) : !info.isFile())
                continue;
            if (!found.contains(info.absoluteFilePath()))
                found.append(info.absoluteFilePath());
        }
    }
    found.sort(Qt::CaseInsensitive);
    return found;
}

QString loadablePathFor(AudioPluginFormat format, const QString &bundlePath)
{
    if (format == AudioPluginFormat::Vst3)
        return Vst3Module::resolveBinaryPath(bundlePath);
    const QFileInfo info(bundlePath);
    return info.isFile() ? info.absoluteFilePath() : QString();
}

bool formatOf(const QString &path, AudioPluginFormat &format)
{
    if (path.endsWith(QLatin1String(".clap"), Qt::CaseInsensitive)) {
        format = AudioPluginFormat::Clap;
        return true;
    }
    if (path.endsWith(QLatin1String(".vst3"), Qt::CaseInsensitive)) {
        format = AudioPluginFormat::Vst3;
        return true;
    }
    return false;
}

std::shared_ptr<AudioPluginModule> openModule(AudioPluginFormat format,
                                              const QString &bundlePath, PluginError &error)
{
    // The last gate before somebody else's constructors run. Deliberately here
    // rather than only in the callers: this function is the single door to
    // dlopen for the whole subsystem, so the check cannot be forgotten by a
    // future caller.
    const QString loadable = loadablePathFor(format, bundlePath);
    if (loadable.isEmpty()) {
        error = makePluginError(PluginErrorCode::BadBundle,
                                tr("This plugin does not contain a version for this kind of "
                                   "computer."));
        return nullptr;
    }
    const QString resolved = AudioPluginTrust::resolve(loadable);
    if (resolved.isEmpty()) {
        error = makePluginError(PluginErrorCode::UntrustedPath,
                                tr("The plugin file is no longer there."));
        return nullptr;
    }
    if (const PluginError pathError = AudioPluginTrust::checkPluginPath(resolved)) {
        error = pathError;
        return nullptr;
    }

    if (format == AudioPluginFormat::Clap)
        return ClapModule::open(resolved, error);
    return Vst3Module::open(bundlePath, error);
}

QList<AudioPluginDescriptor> describe(AudioPluginFormat format, const QString &bundlePath,
                                      PluginError &error)
{
    const std::shared_ptr<AudioPluginModule> module = openModule(format, bundlePath, error);
    if (!module)
        return {};

    QList<AudioPluginDescriptor> descriptors = module->descriptors();

    // Stamp what the file looked like as it was read, so consent recorded from
    // this scan is consent for these exact bytes.
    const QString loadable = loadablePathFor(format, bundlePath);
    const QString sha256 = AudioPluginTrust::sha256OfFile(loadable);
    const qint64 size = AudioPluginTrust::fileSizeOf(loadable);
    for (AudioPluginDescriptor &descriptor : descriptors) {
        descriptor.sha256 = sha256;
        descriptor.fileSize = size;
    }
    return descriptors;
}

} // namespace PluginScanner

} // namespace OpenChat
