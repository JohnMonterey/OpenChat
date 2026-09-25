#pragma once

#include "core/Result.h"
#include "repositories/AttachmentRepository.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace OpenChat {

enum class AttachmentFileError {
    InvalidInput, // an index or body outside what a part can be
    NotFound,     // no file, or no bytes where the part would be
    ReadFailed,
    WriteFailed,  // a full disk among others: reported, never fatal
};

// Where chat attachments' bytes live (docs/chat-attachments.md): one file per
// attachment under <profile directory>/attachments/, holding its parts as
// they travel, sealed under the attachment's key. Part i is at offset
// i × (partBytes + sealOverhead), so parts can be written in any order and
// the file only ever holds what arrived. Plaintext never touches the disk:
// the key lives in the encrypted database, next to the message.
//
// A file is named by the first 32 hex digits of SHA-256("OCAT1" ‖
// conversation ‖ sender device ‖ attachment id), so the directory says
// nothing about who sent what to whom. Removing the profile (or the local
// data reset) removes the directory with it.
//
// Every call opens and closes its own file, so it is safe from any thread;
// two writers of one attachment's parts must not overlap in time.
class AttachmentFileStore final
{
public:
    // `directory` is created on the first write.
    explicit AttachmentFileStore(QString directory);

    [[nodiscard]] QString directory() const { return m_directory; }
    [[nodiscard]] static QString fileNameFor(const AttachmentRef &ref);
    [[nodiscard]] QString pathFor(const AttachmentRef &ref) const;
    // Where part `index` starts.
    [[nodiscard]] static qint64 partOffset(int index);

    // Stores one part's sealed body (ciphertext and tag), replacing what
    // was there.
    [[nodiscard]] Result<void, AttachmentFileError>
    writePart(const AttachmentRef &ref, int index, QByteArrayView sealedBody) const;
    // The `size` bytes stored for part `index`.
    [[nodiscard]] Result<QByteArray, AttachmentFileError>
    readPart(const AttachmentRef &ref, int index, qsizetype size) const;
    // Removes the attachment's file; one that is not there is no error.
    [[nodiscard]] Result<void, AttachmentFileError> remove(const AttachmentRef &ref) const;
    [[nodiscard]] bool contains(const AttachmentRef &ref) const;
    // What every attachment file takes on disk.
    [[nodiscard]] Result<qint64, AttachmentFileError> totalBytes() const;
    // What the volume holding the directory has free; -1 when it cannot say.
    [[nodiscard]] qint64 freeBytes() const;

private:
    QString m_directory;
};

} // namespace OpenChat
