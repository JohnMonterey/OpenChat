#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

#include <optional>
#include <utility>

namespace OpenChat {

// A photo, video, audio clip or file sent in a chat (docs/chat-attachments.md).
//
// It travels in two parts. The visible message is an ordinary MLS message
// (MessageContent type Attachment) carrying an AttachmentDescriptor: sizes, the
// SHA-256 of the whole blob and a fresh random key. The bytes follow as frames
// (EnvelopeMessageKind::AttachmentControl) sealed with that key, outside the
// ratchet, so a part can be sent again byte for byte and parts may arrive in
// any order. The MLS-authenticated hash is what makes the assembled blob
// trustworthy; the key keeps it private and lets a receiver drop a forged or
// damaged frame on sight.
enum class AttachmentKind : quint8 {
    Image = 1, // a baseline JPEG
    Video = 2, // a video sequence (encodeVideoSequence) of clip segments
    Audio = 3, // one SongContainer, within chatSongLimits()
    File = 4,  // the bytes as they were; never opened by OpenChat
};

namespace AttachmentLimits {
// Plaintext bytes per part; every part but the last is exactly this long.
inline constexpr qsizetype partBytes = 224 * 1024;
// The GCM tag each sealed body carries.
inline constexpr qsizetype sealOverhead = 16;
// A Preview, Request or Cancel body also carries its own random 12-byte
// nonce ahead of the ciphertext (see the frame layout below).
inline constexpr qsizetype controlNonceBytes = 12;
inline constexpr qsizetype controlSealOverhead = sealOverhead + controlNonceBytes;
// The largest frame handed to the engine, header and tag included.
inline constexpr qsizetype maxFrameBytes = 240 * 1024;
inline constexpr qint64 maxImageBytes = 2LL * 1024 * 1024;
inline constexpr int maxImageLongSide = 2048;   // what this version encodes
inline constexpr int maxImageDimension = 8192;  // what it accepts
inline constexpr qint64 maxVideoMs = 60 * 1000; // longer sources are trimmed
inline constexpr int maxVideoSegments = 16;
inline constexpr qint64 maxVideoBytes = maxVideoSegments * (224LL * 1024 + 64) + 64;
inline constexpr qint64 maxAudioMs = 5 * 60 * 1000; // longer sources are trimmed
inline constexpr qint64 maxAudioBytes = 4LL * 1024 * 1024;
inline constexpr qint64 maxFileBytes = 16LL * 1024 * 1024;
inline constexpr int maxParts = int((maxFileBytes + partBytes - 1) / partBytes); // 74
inline constexpr qsizetype maxPreviewBytes = 16 * 1024;
inline constexpr int maxPreviewSide = 320;
inline constexpr int maxPeaks = 96;
inline constexpr int maxFileNameLength = 120; // UTF-16 units once sanitised
inline constexpr int maxExtensionLength = 16;
inline constexpr int maxMimeLength = 100;
inline constexpr int maxStaged = 10;
// byteCount × recipients: what one send may put on the wire.
inline constexpr qint64 maxFanOutBytes = 256LL * 1024 * 1024;
inline constexpr qsizetype keyBytes = 32;
// The largest MessageContent an attachment message may encode to: the
// longest caption (the composer's 65 536 UTF-16 units at three bytes each)
// plus a reply's quote, and room for the descriptor and CBOR framing. Checked
// by the sender before encrypting and by every receiver before parsing.
inline constexpr qsizetype maxMessageBytes = 3 * (65536 + 200) + 4096;
static_assert(maxMessageBytes < maxFrameBytes);
static_assert(partBytes + sealOverhead + 64 <= maxFrameBytes);
static_assert(maxParts <= 1024);
} // namespace AttachmentLimits

struct AttachmentDescriptor final {
    AttachmentId attachmentId = AttachmentId::generate();
    QByteArray key;                // AttachmentLimits::keyBytes; never leaves C++
    AttachmentKind kind = AttachmentKind::File;
    qint64 byteCount = 0;          // 1 … attachmentByteCap(kind)
    QByteArray sha256;             // 32 bytes, of the whole plaintext blob
    int partCount = 0;             // attachmentPartCount(byteCount)
    QString mimeType;              // isValidMimeType, or empty
    QString fileName;              // sanitizeAttachmentFileName; may be empty
    int width = 0;                 // Image and Video; 0 when unknown
    int height = 0;
    qint64 durationMs = 0;         // Video and Audio
    bool hasPreview = false;       // a Preview frame follows the descriptor
    QByteArray peaks;              // Audio waveform, 0…255 per bar, at most maxPeaks bars

    friend bool operator==(const AttachmentDescriptor &, const AttachmentDescriptor &) = default;
};

// The largest blob `kind` may carry.
[[nodiscard]] qint64 attachmentByteCap(AttachmentKind kind);
// ceil(byteCount / partBytes); 0 for anything below 1.
[[nodiscard]] int attachmentPartCount(qint64 byteCount);
// The plaintext length of part `index` of `descriptor`: partBytes, or the
// remainder for the last one; 0 for an index outside the attachment.
[[nodiscard]] qsizetype attachmentPartSize(const AttachmentDescriptor &descriptor, int index);

// A file name as a peer may present it: NFC; no control, format, separator,
// private-use or unassigned characters and no lone surrogates; whitespace
// collapsed; <>:"/\|?* replaced by '_'; no leading or trailing dots or
// spaces; a Windows device name (CON, NUL, COM1, …) prefixed with '_'; at most
// maxFileNameLength units, cut without splitting a surrogate pair and keeping
// an extension of up to maxExtensionLength characters. Receivers apply it
// again to whatever arrives.
[[nodiscard]] QString sanitizeAttachmentFileName(const QString &name);
// type/subtype in the RFC 6838 token grammar, at most maxMimeLength.
[[nodiscard]] bool isValidMimeType(const QString &mimeType);
// Every bound above, the kind's cap, partCount == attachmentPartCount, a
// 32-byte key and hash, dimensions and duration in range. A name must be one
// sanitizeAttachmentFileName leaves as it is. Only Image and Video have a
// width and height, only Video and Audio a duration (up to the kind's cap and
// a second more, for an encoder's last frame), only Audio peaks; anything
// else must be zero or empty.
[[nodiscard]] bool isValidDescriptor(const AttachmentDescriptor &descriptor);

// What stands in for an attachment where only text fits: "Photo", "Video",
// "Audio", or the file's name ("File" when it has none).
[[nodiscard]] QString attachmentLabel(AttachmentKind kind, const QString &fileName);
// The label, or "Photo: caption" when there is a caption (collapsed to one
// line). Notifications, reply quotes and the compose bar use it.
[[nodiscard]] QString attachmentSummary(AttachmentKind kind, const QString &fileName,
                                        const QString &caption);

// --- Frames (the ciphertext of an AttachmentControl envelope)
//
//   0      0xAC            1   version (1)      2   type (AttachmentFrameType)
//   3…18   attachment id   19…22  index (u32, big-endian; 0 unless a Part)
//   23…    the sealed body: AES-256-GCM under the descriptor's key, the 23
//          header bytes as associated data, ciphertext then the 16-byte tag.
//
// A Part's nonce is 00 00 00 00 ‖ u32 type ‖ u32 index: each part is sealed
// once and sent again byte for byte, never re-encrypted. Every other type can
// be sealed again with different content under the same key (a receiver asks
// again for fewer parts; each member of a group asks for its own), so its body
// starts with a fresh random 12-byte nonce whose top bit is set, keeping it out
// of the Part nonces' space: nonce ‖ ciphertext ‖ tag.
//
// Bodies: Part = that part's bytes; Preview = a baseline JPEG; Request = a
// bitmap of the parts still missing (bit i of byte i/8, least significant
// first); Cancel = nothing.
enum class AttachmentFrameType : quint8 {
    Part = 1,
    Preview = 2,
    Request = 3,
    Cancel = 4,
};

inline constexpr qsizetype attachmentFrameHeaderBytes = 23;

struct AttachmentFrameHeader final {
    AttachmentFrameType type = AttachmentFrameType::Part;
    AttachmentId attachmentId = AttachmentId::generate();
    quint32 index = 0;
};

// The 23 header bytes (also the sealed body's associated data).
[[nodiscard]] QByteArray attachmentFrameHeader(AttachmentFrameType type,
                                               const AttachmentId &attachmentId, quint32 index);
// The header and the sealed body, or nothing for a frame over maxFrameBytes,
// of another version or an unknown type, a Part index of maxParts or more, a
// non-zero index on any other type, or a body shorter than a tag (than a nonce
// and a tag, for any type but a Part).
[[nodiscard]] std::optional<std::pair<AttachmentFrameHeader, QByteArray>>
splitAttachmentFrame(QByteArrayView frame);

// --- Blobs

// A video attachment: the magic "OCVS", then CBOR [1, [segment, …]], every
// segment an encoded ClipContainer, 1 … maxVideoSegments of them, all the
// same size and rate. Encoded canonically, and read back only in that form.
[[nodiscard]] QByteArray encodeVideoSequence(const QVector<QByteArray> &segments);
// The segments, each already through decodeClipContainer; nothing for a
// malformed sequence or any segment that does not decode.
[[nodiscard]] std::optional<QVector<QByteArray>> decodeVideoSequence(QByteArrayView blob);

// What a receiver checks once every part is in: the size and SHA-256 the
// descriptor names, then per kind: Image a JPEG with 1 … maxJpegScans scans
// whose frame size matches the descriptor and stays within
// maxImageDimension; Video decodeVideoSequence; Audio
// decodeSongContainer(blob, chatSongLimits()); File nothing more.
[[nodiscard]] bool attachmentBlobIsValid(const AttachmentDescriptor &descriptor,
                                         QByteArrayView blob);
// A preview as a receiver will show it: a JPEG within maxPreviewBytes, with
// 1 … maxJpegScans scans and a long side of at most maxPreviewSide.
[[nodiscard]] bool previewIsAcceptable(QByteArrayView jpeg);
// The width and height a JPEG's first frame header declares, or nothing.
[[nodiscard]] std::optional<std::pair<int, int>> jpegFrameSize(QByteArrayView jpeg);

} // namespace OpenChat
