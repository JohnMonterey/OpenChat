#pragma once

#include "domain/Attachment.h"

#include <QByteArray>
#include <QByteArrayView>

#include <optional>

namespace OpenChat {

// Seals and opens chat attachment frames (the layout is in
// domain/Attachment.h): AES-256-GCM under the attachment's own key, with a
// nonce made of the frame's type and index (so no two frames under one key
// share one) and the frame header as associated data (so a body cannot be
// moved to another attachment, index or type).

// A fresh random key (AttachmentLimits::keyBytes) from the system CSPRNG;
// empty if the generator fails, which callers treat as "cannot send".
[[nodiscard]] QByteArray randomAttachmentKey();

// The whole frame: header then sealed body. Empty for a key of the wrong size
// or a body the frame could not carry (over maxFrameBytes when sealed).
[[nodiscard]] QByteArray sealAttachmentFrame(const QByteArray &key, AttachmentFrameType type,
                                             const AttachmentId &attachmentId, quint32 index,
                                             QByteArrayView plaintext);

// The plaintext of a frame's body, or nothing when the frame is malformed,
// the key is wrong, or anything (header included) was changed.
[[nodiscard]] std::optional<QByteArray> openAttachmentFrame(const QByteArray &key,
                                                            QByteArrayView frame);
// The same for a body kept apart from its header (the file store keeps only
// bodies): `header` is what attachmentFrameHeader returns for it.
[[nodiscard]] std::optional<QByteArray> openAttachmentBody(const QByteArray &key,
                                                           QByteArrayView header,
                                                           QByteArrayView sealedBody);

} // namespace OpenChat
