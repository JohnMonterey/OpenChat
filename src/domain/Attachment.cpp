#include "domain/Attachment.h"

#include "domain/ClipContainer.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"

#include <QCborArray>
#include <QCborValue>
#include <QCryptographicHash>
#include <QtEndian>

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace OpenChat {

namespace {

constexpr quint8 frameMagic = 0xAC;
constexpr quint8 frameVersion = 1;
constexpr quint8 lastFrameType = quint8(AttachmentFrameType::Cancel);

// A video sequence: the magic, then CBOR [version, [segment bstr, …]].
constexpr char videoMagic[4] = {'O', 'C', 'V', 'S'};
constexpr qint64 videoSequenceVersion = 1;

// Room for an encoder's last frame or a segment boundary past the nominal
// cap: a five-minute song may run to chatSongLimits().maxTotalSamples.
constexpr qint64 durationSlackMs = 1000;

// The Windows device names a file may not be called, whatever its extension.
constexpr const char16_t *reservedNames[] = {
    u"CON",  u"PRN",  u"AUX",  u"NUL",  u"COM1", u"COM2", u"COM3", u"COM4", u"COM5", u"COM6", u"COM7",
    u"COM8", u"COM9", u"LPT1", u"LPT2", u"LPT3", u"LPT4", u"LPT5", u"LPT6", u"LPT7", u"LPT8", u"LPT9",
};

[[nodiscard]] bool isKnownKind(AttachmentKind kind) noexcept
{
    return kind == AttachmentKind::Image || kind == AttachmentKind::Video
        || kind == AttachmentKind::Audio || kind == AttachmentKind::File;
}

[[nodiscard]] bool hasPicture(AttachmentKind kind) noexcept
{
    return kind == AttachmentKind::Image || kind == AttachmentKind::Video;
}

[[nodiscard]] qint64 maxDurationFor(AttachmentKind kind) noexcept
{
    switch (kind) {
    case AttachmentKind::Video:
        return AttachmentLimits::maxVideoMs + durationSlackMs;
    case AttachmentKind::Audio:
        return AttachmentLimits::maxAudioMs + durationSlackMs;
    case AttachmentKind::Image:
    case AttachmentKind::File:
        break;
    }
    return 0;
}

[[nodiscard]] bool looksLikeJpeg(QByteArrayView data) noexcept
{
    return data.size() >= 3 && quint8(data[0]) == 0xFF && quint8(data[1]) == 0xD8 && quint8(data[2]) == 0xFF;
}

// The dropped characters of a file name: controls, format characters (bidi
// overrides, zero-width joiners, tags), line and paragraph separators,
// private-use, unassigned and surrogate code points.
[[nodiscard]] bool isDroppedFromName(char32_t c) noexcept
{
    switch (QChar::category(c)) {
    case QChar::Other_Control:
    case QChar::Other_Format:
    case QChar::Other_Surrogate:
    case QChar::Other_PrivateUse:
    case QChar::Other_NotAssigned:
    case QChar::Separator_Line:
    case QChar::Separator_Paragraph:
        return true;
    default:
        return false;
    }
}

// Characters Windows refuses in a name (':' would also name an alternate
// data stream); '/' and '\' would make a path of it anywhere.
[[nodiscard]] bool isReplacedInName(char32_t c) noexcept
{
    return c == U'<' || c == U'>' || c == U':' || c == U'"' || c == U'/' || c == U'\\' || c == U'|'
        || c == U'?' || c == U'*';
}

// A tab or line break between two words becomes a space rather than gluing
// them together; every other control is dropped.
[[nodiscard]] bool isSpaceInName(char32_t c) noexcept
{
    return c == U'\t' || c == U'\n' || c == U'\v' || c == U'\f' || c == U'\r' || c == 0x85
        || (QChar::category(c) == QChar::Separator_Space);
}

[[nodiscard]] QString trimDotsAndSpaces(const QString &name)
{
    qsizetype first = 0;
    qsizetype last = name.size();
    while (first < last && (name.at(first) == u'.' || name.at(first) == u' '))
        ++first;
    while (last > first && (name.at(last - 1) == u'.' || name.at(last - 1) == u' '))
        --last;
    return name.sliced(first, last - first);
}

// At most `length` units of `text`, one fewer if the cut would split a
// surrogate pair.
[[nodiscard]] QString leftWhole(const QString &text, qsizetype length)
{
    length = std::min(length, text.size());
    if (length > 0 && length < text.size() && text.at(length - 1).isHighSurrogate())
        --length;
    return text.left(length);
}

[[nodiscard]] QString truncatedName(const QString &name)
{
    constexpr qsizetype limit = AttachmentLimits::maxFileNameLength;
    if (name.size() <= limit)
        return name;
    const qsizetype dot = name.lastIndexOf(u'.');
    const qsizetype extensionLength = dot < 0 ? 0 : name.size() - dot - 1;
    if (dot > 0 && extensionLength >= 1 && extensionLength <= AttachmentLimits::maxExtensionLength) {
        const QString extension = name.sliced(dot); // with its dot
        const QString base = trimDotsAndSpaces(leftWhole(name.left(dot), limit - extension.size()));
        if (!base.isEmpty())
            return base + extension;
    }
    return leftWhole(name, limit);
}

[[nodiscard]] bool isReservedDeviceName(const QString &name)
{
    // Windows ignores the extension and trailing spaces: "nul .txt" is NUL.
    const QString base = name.section(u'.', 0, 0).trimmed();
    return std::any_of(std::begin(reservedNames), std::end(reservedNames), [&](const char16_t *reserved) {
        return base.compare(QStringView(reserved), Qt::CaseInsensitive) == 0;
    });
}

// The first SOF segment's (width, height), walking marker segments the way
// jpegScanCount does. Every SOF marker counts (baseline, progressive,
// lossless, arithmetic); DHT (C4), JPG (C8) and DAC (CC) share the range but
// are not frames.
[[nodiscard]] bool isStartOfFrame(quint8 marker) noexcept
{
    return marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

} // namespace

qint64 attachmentByteCap(AttachmentKind kind)
{
    switch (kind) {
    case AttachmentKind::Image:
        return AttachmentLimits::maxImageBytes;
    case AttachmentKind::Video:
        return AttachmentLimits::maxVideoBytes;
    case AttachmentKind::Audio:
        return AttachmentLimits::maxAudioBytes;
    case AttachmentKind::File:
        return AttachmentLimits::maxFileBytes;
    }
    return 0;
}

int attachmentPartCount(qint64 byteCount)
{
    if (byteCount < 1)
        return 0;
    // Written so a hostile byteCount near the top of qint64 cannot overflow.
    const qint64 parts = (byteCount - 1) / AttachmentLimits::partBytes + 1;
    return int(std::min<qint64>(parts, std::numeric_limits<int>::max()));
}

qsizetype attachmentPartSize(const AttachmentDescriptor &descriptor, int index)
{
    const int parts = attachmentPartCount(descriptor.byteCount);
    if (index < 0 || index >= parts)
        return 0;
    if (index < parts - 1)
        return AttachmentLimits::partBytes;
    return qsizetype(descriptor.byteCount - qint64(parts - 1) * AttachmentLimits::partBytes);
}

QString sanitizeAttachmentFileName(const QString &name)
{
    // Characters are dropped before normalising, so what survives on either
    // side of a dropped one is composed as it will finally be shown.
    QString kept;
    kept.reserve(name.size());
    const qsizetype size = name.size();
    for (qsizetype i = 0; i < size; ++i) {
        const QChar unit = name.at(i);
        char32_t c = unit.unicode();
        if (unit.isHighSurrogate()) {
            if (i + 1 >= size || !name.at(i + 1).isLowSurrogate())
                continue; // a lone surrogate
            c = QChar::surrogateToUcs4(unit, name.at(i + 1));
            ++i;
        } else if (unit.isLowSurrogate()) {
            continue;
        }
        if (isSpaceInName(c)) {
            kept.append(u' ');
            continue;
        }
        if (isDroppedFromName(c))
            continue;
        if (isReplacedInName(c)) {
            kept.append(u'_');
            continue;
        }
        kept.append(QString::fromUcs4(&c, 1));
    }
    kept = kept.normalized(QString::NormalizationForm_C);

    // Runs of whitespace (and whatever used to separate them) become one
    // space.
    QString collapsed;
    collapsed.reserve(kept.size());
    for (const QChar unit : std::as_const(kept)) {
        if (unit != u' ' || !collapsed.endsWith(u' '))
            collapsed.append(unit);
    }

    QString result = trimDotsAndSpaces(truncatedName(trimDotsAndSpaces(collapsed)));
    if (isReservedDeviceName(result))
        result.prepend(u'_');
    return result;
}

bool isValidMimeType(const QString &mimeType)
{
    // RFC 6838 §4.2: restricted-name = first char alphanumeric, then up to
    // 126 of alphanumerics and ! # $ & - ^ _ . + ; one type, one subtype, no
    // parameters.
    if (mimeType.isEmpty() || mimeType.size() > AttachmentLimits::maxMimeLength)
        return false;
    const auto isAlnum = [](QChar c) {
        return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9');
    };
    const auto isNameChar = [&](QChar c) {
        return isAlnum(c) || c == u'!' || c == u'#' || c == u'$' || c == u'&' || c == u'-' || c == u'^'
            || c == u'_' || c == u'.' || c == u'+';
    };
    const auto isRestrictedName = [&](QStringView part) {
        return !part.isEmpty() && part.size() <= 127 && isAlnum(part.front())
            && std::all_of(part.begin(), part.end(), isNameChar);
    };
    const qsizetype slash = mimeType.indexOf(u'/');
    if (slash < 0 || mimeType.indexOf(u'/', slash + 1) >= 0)
        return false;
    const QStringView view(mimeType);
    return isRestrictedName(view.first(slash)) && isRestrictedName(view.sliced(slash + 1));
}

bool isValidDescriptor(const AttachmentDescriptor &descriptor)
{
    const AttachmentKind kind = descriptor.kind;
    if (!isKnownKind(kind) || descriptor.key.size() != AttachmentLimits::keyBytes
        || descriptor.sha256.size() != 32)
        return false;
    if (descriptor.byteCount < 1 || descriptor.byteCount > attachmentByteCap(kind)
        || descriptor.partCount != attachmentPartCount(descriptor.byteCount)
        || descriptor.partCount > AttachmentLimits::maxParts)
        return false;
    if (!descriptor.mimeType.isEmpty() && !isValidMimeType(descriptor.mimeType))
        return false;
    // Only a name this function would leave alone: receivers clean it again,
    // and an honest sender's name must survive that unchanged.
    if (descriptor.fileName.size() > AttachmentLimits::maxFileNameLength
        || sanitizeAttachmentFileName(descriptor.fileName) != descriptor.fileName)
        return false;
    const auto inRange = [](qint64 value, qint64 maximum) { return value >= 0 && value <= maximum; };
    const int maxSide = hasPicture(kind) ? AttachmentLimits::maxImageDimension : 0;
    if (!inRange(descriptor.width, maxSide) || !inRange(descriptor.height, maxSide))
        return false;
    if (!inRange(descriptor.durationMs, maxDurationFor(kind)))
        return false;
    const qsizetype maxPeaks = kind == AttachmentKind::Audio ? AttachmentLimits::maxPeaks : 0;
    return descriptor.peaks.size() <= maxPeaks;
}

QString attachmentLabel(AttachmentKind kind, const QString &fileName)
{
    switch (kind) {
    case AttachmentKind::Image:
        return QStringLiteral("Photo");
    case AttachmentKind::Video:
        return QStringLiteral("Video");
    case AttachmentKind::Audio:
        return QStringLiteral("Audio");
    case AttachmentKind::File:
        break;
    }
    return fileName.isEmpty() ? QStringLiteral("File") : fileName;
}

QString attachmentSummary(AttachmentKind kind, const QString &fileName, const QString &caption)
{
    const QString label = attachmentLabel(kind, fileName);
    const QString line = caption.simplified();
    return line.isEmpty() ? label : label + QStringLiteral(": ") + line;
}

// --- Frames ---------------------------------------------------------------

QByteArray attachmentFrameHeader(AttachmentFrameType type, const AttachmentId &attachmentId,
                                 quint32 index)
{
    QByteArray header;
    header.reserve(attachmentFrameHeaderBytes);
    header.append(char(frameMagic));
    header.append(char(frameVersion));
    header.append(char(quint8(type)));
    header.append(attachmentId.bytes());
    char indexBytes[4];
    qToBigEndian(index, indexBytes);
    header.append(indexBytes, sizeof(indexBytes));
    return header;
}

std::optional<std::pair<AttachmentFrameHeader, QByteArray>> splitAttachmentFrame(QByteArrayView frame)
{
    if (frame.size() > AttachmentLimits::maxFrameBytes
        || frame.size() < attachmentFrameHeaderBytes + AttachmentLimits::sealOverhead)
        return std::nullopt;
    if (quint8(frame[0]) != frameMagic || quint8(frame[1]) != frameVersion)
        return std::nullopt;
    const quint8 type = quint8(frame[2]);
    if (type < quint8(AttachmentFrameType::Part) || type > lastFrameType)
        return std::nullopt;
    const auto attachmentId = AttachmentId::fromBytes(frame.sliced(3, AttachmentId::byteCount));
    if (!attachmentId)
        return std::nullopt;
    const quint32 index = qFromBigEndian<quint32>(frame.data() + 19);
    if (type == quint8(AttachmentFrameType::Part) ? index >= quint32(AttachmentLimits::maxParts)
                                                  : index != 0)
        return std::nullopt;
    // Every type but a Part carries its own nonce ahead of the tag.
    if (type != quint8(AttachmentFrameType::Part)
        && frame.size() < attachmentFrameHeaderBytes + AttachmentLimits::controlSealOverhead)
        return std::nullopt;

    AttachmentFrameHeader header;
    header.type = AttachmentFrameType(type);
    header.attachmentId = *attachmentId;
    header.index = index;
    return std::pair{header, frame.sliced(attachmentFrameHeaderBytes).toByteArray()};
}

// --- Blobs ----------------------------------------------------------------

QByteArray encodeVideoSequence(const QVector<QByteArray> &segments)
{
    if (segments.isEmpty() || segments.size() > AttachmentLimits::maxVideoSegments)
        return {};
    std::optional<ClipContainer> first;
    QCborArray list;
    for (const QByteArray &segment : segments) {
        const auto clip = decodeClipContainer(segment);
        if (!clip)
            return {};
        if (!first)
            first = clip;
        else if (clip->width != first->width || clip->height != first->height || clip->fps != first->fps)
            return {};
        list.append(segment);
    }
    QByteArray out(videoMagic, sizeof(videoMagic));
    out.append(QCborValue(QCborArray{videoSequenceVersion, list}).toCbor());
    if (out.size() > AttachmentLimits::maxVideoBytes)
        return {};
    return out;
}

std::optional<QVector<QByteArray>> decodeVideoSequence(QByteArrayView blob)
{
    const QByteArrayView magic(videoMagic, sizeof(videoMagic));
    if (blob.size() <= magic.size() || blob.size() > AttachmentLimits::maxVideoBytes
        || blob.first(magic.size()) != magic)
        return std::nullopt;
    const QByteArray body = blob.sliced(magic.size()).toByteArray();
    QCborParserError error;
    const QCborValue root = QCborValue::fromCbor(body, &error);
    if (error.error != QCborError::NoError || !root.isArray())
        return std::nullopt;
    // Exactly what encodeVideoSequence writes, nothing appended: a sequence
    // has one encoding, so the bytes the hash covers are the bytes played.
    if (root.toCbor() != body)
        return std::nullopt;
    const QCborArray fields = root.toArray();
    if (fields.size() != 2 || !fields.at(0).isInteger() || fields.at(0).toInteger() != videoSequenceVersion
        || !fields.at(1).isArray())
        return std::nullopt;
    const QCborArray list = fields.at(1).toArray();
    if (list.isEmpty() || list.size() > AttachmentLimits::maxVideoSegments)
        return std::nullopt;

    QVector<QByteArray> segments;
    segments.reserve(list.size());
    std::optional<ClipContainer> first;
    for (const QCborValue &value : list) {
        if (!value.isByteArray())
            return std::nullopt;
        QByteArray segment = value.toByteArray();
        const auto clip = decodeClipContainer(segment);
        if (!clip)
            return std::nullopt;
        if (!first)
            first = clip;
        else if (clip->width != first->width || clip->height != first->height || clip->fps != first->fps)
            return std::nullopt;
        segments.push_back(std::move(segment));
    }
    return segments;
}

bool attachmentBlobIsValid(const AttachmentDescriptor &descriptor, QByteArrayView blob)
{
    if (!isValidDescriptor(descriptor) || blob.size() != descriptor.byteCount
        || QCryptographicHash::hash(blob, QCryptographicHash::Sha256) != descriptor.sha256)
        return false;

    switch (descriptor.kind) {
    case AttachmentKind::Image: {
        // A progressive "scan bomb" re-reads the whole picture per scan, and
        // a frame larger than described would break the bubble's layout.
        if (!looksLikeJpeg(blob))
            return false;
        const int scans = jpegScanCount(blob);
        if (scans < 1 || scans > maxJpegScans)
            return false;
        const auto size = jpegFrameSize(blob);
        if (!size || size->first > AttachmentLimits::maxImageDimension
            || size->second > AttachmentLimits::maxImageDimension)
            return false;
        // A frame the decoder must hold whole costs memory for its full size
        // however small it is drawn (8192 px square progressive: ~400 MiB);
        // this version only sends single-scan baseline, so it is taken only
        // as large as that.
        if (jpegIsBuffered(blob)
            && std::max(size->first, size->second) > AttachmentLimits::maxBufferedImageSide)
            return false;
        const bool described = descriptor.width != 0 || descriptor.height != 0;
        return !described || (size->first == descriptor.width && size->second == descriptor.height);
    }
    case AttachmentKind::Video:
        return decodeVideoSequence(blob).has_value();
    case AttachmentKind::Audio:
        return decodeSongContainer(blob, chatSongLimits()).has_value();
    case AttachmentKind::File:
        return true;
    }
    return false;
}

bool previewIsAcceptable(QByteArrayView jpeg)
{
    if (jpeg.size() > AttachmentLimits::maxPreviewBytes || !looksLikeJpeg(jpeg))
        return false;
    const int scans = jpegScanCount(jpeg);
    if (scans < 1 || scans > maxJpegScans)
        return false;
    const auto size = jpegFrameSize(jpeg);
    return size && std::max(size->first, size->second) <= AttachmentLimits::maxPreviewSide;
}

std::optional<std::pair<int, int>> jpegFrameSize(QByteArrayView jpeg)
{
    const auto frame = jpegFrame(jpeg);
    if (!frame)
        return std::nullopt;
    return std::pair{frame->width, frame->height};
}

bool jpegIsBuffered(QByteArrayView jpeg)
{
    const auto frame = jpegFrame(jpeg);
    // Baseline (C0) and extended sequential (C1) Huffman frames in one scan
    // stream through the decoder a few rows at a time; everything else
    // (progressive, a scan per component, arithmetic, lossless) keeps the
    // whole frame's coefficients.
    return !frame || (frame->marker != 0xC0 && frame->marker != 0xC1) || jpegScanCount(jpeg) != 1;
}

std::optional<JpegFrame> jpegFrame(QByteArrayView jpeg)
{
    const auto byteAt = [&](qsizetype index) { return static_cast<quint8>(jpeg[index]); };
    const qsizetype size = jpeg.size();
    if (size < 4 || byteAt(0) != 0xFF || byteAt(1) != 0xD8)
        return std::nullopt;

    constexpr quint8 startOfScan = 0xDA, endOfImage = 0xD9, startOfImage = 0xD8;
    qsizetype pos = 2;
    while (true) {
        // A marker: 0xFF, any number of 0xFF fill bytes, then the code.
        if (pos >= size || byteAt(pos) != 0xFF)
            return std::nullopt;
        while (pos < size && byteAt(pos) == 0xFF)
            ++pos;
        if (pos >= size)
            return std::nullopt;
        const quint8 marker = byteAt(pos++);
        // The frame header comes before the first scan; a scan or the end
        // without one is not a picture.
        if (marker == endOfImage || marker == startOfScan || marker == startOfImage || marker == 0x00)
            return std::nullopt;
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) // no length
            continue;

        if (pos + 2 > size)
            return std::nullopt;
        const qsizetype length = (qsizetype(byteAt(pos)) << 8) | byteAt(pos + 1);
        if (length < 2 || pos + length > size)
            return std::nullopt;
        if (isStartOfFrame(marker)) {
            // length(2) precision(1) height(2) width(2) …
            if (length < 7)
                return std::nullopt;
            const int height = (int(byteAt(pos + 3)) << 8) | byteAt(pos + 4);
            const int width = (int(byteAt(pos + 5)) << 8) | byteAt(pos + 6);
            // A height of 0 defers to a DNL marker later in the scan; no
            // encoder this side makes one, and nothing here trusts it.
            if (width < 1 || height < 1)
                return std::nullopt;
            return JpegFrame{marker, width, height};
        }
        pos += length;
    }
}

} // namespace OpenChat
