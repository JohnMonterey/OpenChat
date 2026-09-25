#include "app/AttachmentFileStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <utility>

namespace OpenChat {

namespace {

// Where one part's sealed body starts: every slot holds the largest one.
constexpr qint64 slotBytes = AttachmentLimits::partBytes + AttachmentLimits::sealOverhead;
const QString fileSuffix = QStringLiteral(".ocab");

template<typename T>
Result<T, AttachmentFileError> failed(AttachmentFileError error)
{
    return Result<T, AttachmentFileError>::failure(error);
}

} // namespace

AttachmentFileStore::AttachmentFileStore(QString directory)
    : m_directory(std::move(directory))
{
}

QString AttachmentFileStore::fileNameFor(const AttachmentRef &ref)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("OCAT1"));
    hash.addData(ref.conversationId.bytes());
    hash.addData(ref.senderDeviceId.bytes());
    hash.addData(ref.attachmentId.bytes());
    return QString::fromLatin1(hash.result().toHex().left(32)) + fileSuffix;
}

QString AttachmentFileStore::pathFor(const AttachmentRef &ref) const
{
    return QDir(m_directory).filePath(fileNameFor(ref));
}

qint64 AttachmentFileStore::partOffset(int index)
{
    return qint64(index) * slotBytes;
}

Result<void, AttachmentFileError>
AttachmentFileStore::writePart(const AttachmentRef &ref, int index, QByteArrayView sealedBody) const
{
    if (index < 0 || index >= AttachmentLimits::maxParts || sealedBody.isEmpty()
        || sealedBody.size() > slotBytes)
        return failed<void>(AttachmentFileError::InvalidInput);
    if (!QDir().mkpath(m_directory))
        return failed<void>(AttachmentFileError::WriteFailed);
    QFile file(pathFor(ref));
    const bool fresh = !file.exists();
    // ReadWrite keeps what is there (other parts) and creates the file.
    if (!file.open(QIODevice::ReadWrite))
        return failed<void>(AttachmentFileError::WriteFailed);
    if (fresh)
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!file.seek(partOffset(index)) || file.write(sealedBody.data(), sealedBody.size()) != sealedBody.size()
        || !file.flush())
        return failed<void>(AttachmentFileError::WriteFailed);
    file.close();
    if (file.error() != QFileDevice::NoError)
        return failed<void>(AttachmentFileError::WriteFailed);
    return Result<void, AttachmentFileError>::success();
}

Result<QByteArray, AttachmentFileError>
AttachmentFileStore::readPart(const AttachmentRef &ref, int index, qsizetype size) const
{
    if (index < 0 || index >= AttachmentLimits::maxParts || size <= 0 || size > slotBytes)
        return failed<QByteArray>(AttachmentFileError::InvalidInput);
    QFile file(pathFor(ref));
    if (!file.exists())
        return failed<QByteArray>(AttachmentFileError::NotFound);
    if (!file.open(QIODevice::ReadOnly))
        return failed<QByteArray>(AttachmentFileError::ReadFailed);
    const qint64 offset = partOffset(index);
    if (file.size() < offset + size)
        return failed<QByteArray>(AttachmentFileError::NotFound);
    if (!file.seek(offset))
        return failed<QByteArray>(AttachmentFileError::ReadFailed);
    QByteArray body = file.read(size);
    if (body.size() != size)
        return failed<QByteArray>(AttachmentFileError::ReadFailed);
    return Result<QByteArray, AttachmentFileError>::success(std::move(body));
}

Result<void, AttachmentFileError> AttachmentFileStore::remove(const AttachmentRef &ref) const
{
    const QString path = pathFor(ref);
    if (!QFile::exists(path) || QFile::remove(path))
        return Result<void, AttachmentFileError>::success();
    return failed<void>(AttachmentFileError::WriteFailed);
}

bool AttachmentFileStore::contains(const AttachmentRef &ref) const
{
    return QFile::exists(pathFor(ref));
}

Result<qint64, AttachmentFileError> AttachmentFileStore::totalBytes() const
{
    const QDir directory(m_directory);
    if (!directory.exists())
        return Result<qint64, AttachmentFileError>::success(0);
    qint64 total = 0;
    QDirIterator files(m_directory, {QStringLiteral("*") + fileSuffix}, QDir::Files | QDir::Hidden);
    while (files.hasNext()) {
        files.next();
        total += files.fileInfo().size();
    }
    return Result<qint64, AttachmentFileError>::success(total);
}

qint64 AttachmentFileStore::freeBytes() const
{
    // The directory may not exist yet: its profile directory is on the same
    // volume.
    QString existing = m_directory;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).absolutePath();
        if (parent == existing)
            break;
        existing = parent;
    }
    const QStorageInfo storage(existing);
    if (!storage.isValid() || !storage.isReady())
        return -1;
    return storage.bytesAvailable();
}

} // namespace OpenChat
