#include <QtTest>

#include "domain/Attachment.h"
#include "domain/ClipContainer.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "security/AttachmentSeal.h"

#include <QBuffer>
#include <QCborArray>
#include <QCborValue>
#include <QCryptographicHash>
#include <QImage>
#include <QImageWriter>

#include <functional>
#include <limits>

using namespace OpenChat;

namespace {

QByteArray sha256(QByteArrayView bytes)
{
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

// A descriptor that passes isValidDescriptor for `kind`.
AttachmentDescriptor descriptorFor(AttachmentKind kind, qint64 byteCount = 1000)
{
    AttachmentDescriptor descriptor;
    descriptor.key = QByteArray(AttachmentLimits::keyBytes, 'k');
    descriptor.kind = kind;
    descriptor.byteCount = byteCount;
    descriptor.sha256 = QByteArray(32, 's');
    descriptor.partCount = attachmentPartCount(byteCount);
    switch (kind) {
    case AttachmentKind::Image:
        descriptor.mimeType = QStringLiteral("image/jpeg");
        descriptor.fileName = QStringLiteral("Harbour at dusk.jpg");
        descriptor.width = 2048;
        descriptor.height = 1536;
        descriptor.hasPreview = true;
        break;
    case AttachmentKind::Video:
        descriptor.width = 480;
        descriptor.height = 270;
        descriptor.durationMs = 42'000;
        descriptor.hasPreview = true;
        break;
    case AttachmentKind::Audio:
        descriptor.fileName = QStringLiteral("Voice memo.m4a");
        descriptor.durationMs = 185'000;
        descriptor.peaks = QByteArray(AttachmentLimits::maxPeaks, '\x40');
        break;
    case AttachmentKind::File:
        descriptor.mimeType = QStringLiteral("application/pdf");
        descriptor.fileName = QStringLiteral("Lease agreement (signed).pdf");
        break;
    }
    return descriptor;
}

// A real JPEG from Qt's own encoder: a gradient, or noise, which hardly
// compresses at all.
QByteArray realJpeg(int width, int height, bool progressive = false, int quality = 80, bool noisy = false)
{
    QImage image(width, height, QImage::Format_RGB32);
    quint32 seed = 0x2545F491;
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            seed = seed * 1664525u + 1013904223u;
            line[x] = noisy ? QRgb(0xFF000000u | (seed >> 8))
                            : qRgb(x * 255 / width, y * 255 / height, 128);
        }
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    writer.setProgressiveScanWrite(progressive);
    if (!writer.write(image))
        return {};
    return bytes;
}

// A JPEG that is well formed only as far as the marker walk goes: a frame
// header declaring width × height and `scans` scans of filler.
QByteArray structuralJpeg(int width, int height, int scans, quint8 frameMarker = 0xC2)
{
    QByteArray jpeg("\xFF\xD8", 2);
    const auto segment = [&](quint8 marker, const QByteArray &payload) {
        jpeg.append(char(0xFF));
        jpeg.append(char(marker));
        const int length = int(payload.size()) + 2;
        jpeg.append(char((length >> 8) & 0xFF));
        jpeg.append(char(length & 0xFF));
        jpeg.append(payload);
    };
    segment(0xDB, QByteArray(65, '\x01'));
    QByteArray frame;
    frame.append(char(8));
    frame.append(char((height >> 8) & 0xFF));
    frame.append(char(height & 0xFF));
    frame.append(char((width >> 8) & 0xFF));
    frame.append(char(width & 0xFF));
    frame.append(QByteArray::fromHex("01011100"));
    segment(frameMarker, frame);
    segment(0xC4, QByteArray(28, '\x02'));
    for (int scan = 0; scan < scans; ++scan) {
        segment(0xDA, QByteArray::fromHex("010100003f00"));
        jpeg.append(QByteArray::fromHex("1234ff0056ffd07890"));
    }
    jpeg.append("\xFF\xD9", 2);
    return jpeg;
}

// A clip segment the container accepts (the frames are not real VP9: only
// libvpx would know, and nothing here decodes them).
QByteArray clipSegment(int width = 64, int height = 48, int fps = 15, char fill = 'v')
{
    ClipContainer clip;
    clip.width = width;
    clip.height = height;
    clip.fps = fps;
    clip.durationMs = 1000;
    for (int frame = 0; frame < 10; ++frame)
        clip.frames.push_back({frame == 0, QByteArray(40 + frame, fill)});
    return encodeClipContainer(clip);
}

// A mono song of `seconds` in 60 ms packets of `packetBytes` bytes.
SongContainer songOf(qint64 seconds, int packetBytes = 120)
{
    SongContainer song;
    song.channels = 1;
    song.frameSamples = 2880;
    song.preSkip = 312;
    song.totalSamples = seconds * SongContainer::sampleRate;
    const qint64 packets = (song.totalSamples + song.preSkip + song.frameSamples - 1) / song.frameSamples;
    for (qint64 packet = 0; packet < packets; ++packet)
        song.packets.push_back(QByteArray(packetBytes, char('a' + packet % 26)));
    return song;
}

QByteArray withByte(QByteArray bytes, qsizetype index, quint8 value)
{
    bytes[index] = char(value);
    return bytes;
}

QByteArray flipped(QByteArray bytes, qsizetype index)
{
    bytes[index] = char(bytes[index] ^ 0x01);
    return bytes;
}

} // namespace

// Chat attachments below the app: the descriptor's bounds, the file-name
// cleaning every receiver applies, the frame codec and its AES-GCM seal, and
// the checks an assembled blob or a preview must pass before anything shows.
class AttachmentTest final : public QObject
{
    Q_OBJECT

private slots:
    void partsAreFixedSizeWithARemainderLast()
    {
        QCOMPARE(attachmentPartCount(0), 0);
        QCOMPARE(attachmentPartCount(-5), 0);
        QCOMPARE(attachmentPartCount(1), 1);
        QCOMPARE(attachmentPartCount(AttachmentLimits::partBytes), 1);
        QCOMPARE(attachmentPartCount(AttachmentLimits::partBytes + 1), 2);
        QCOMPARE(attachmentPartCount(AttachmentLimits::maxFileBytes), AttachmentLimits::maxParts);
        QCOMPARE(AttachmentLimits::maxParts, 74);
        // A hostile count cannot overflow into something small.
        QVERIFY(attachmentPartCount(std::numeric_limits<qint64>::max()) > AttachmentLimits::maxParts);

        AttachmentDescriptor descriptor
            = descriptorFor(AttachmentKind::File, 2 * AttachmentLimits::partBytes + 17);
        QCOMPARE(descriptor.partCount, 3);
        QCOMPARE(attachmentPartSize(descriptor, 0), AttachmentLimits::partBytes);
        QCOMPARE(attachmentPartSize(descriptor, 1), AttachmentLimits::partBytes);
        QCOMPARE(attachmentPartSize(descriptor, 2), qsizetype(17));
        QCOMPARE(attachmentPartSize(descriptor, 3), qsizetype(0));
        QCOMPARE(attachmentPartSize(descriptor, -1), qsizetype(0));

        descriptor = descriptorFor(AttachmentKind::File, AttachmentLimits::partBytes);
        QCOMPARE(attachmentPartSize(descriptor, 0), AttachmentLimits::partBytes);
    }

    void eachKindHasItsOwnCap()
    {
        QCOMPARE(attachmentByteCap(AttachmentKind::Image), AttachmentLimits::maxImageBytes);
        QCOMPARE(attachmentByteCap(AttachmentKind::Video), AttachmentLimits::maxVideoBytes);
        QCOMPARE(attachmentByteCap(AttachmentKind::Audio), AttachmentLimits::maxAudioBytes);
        QCOMPARE(attachmentByteCap(AttachmentKind::File), AttachmentLimits::maxFileBytes);
        QCOMPARE(attachmentByteCap(AttachmentKind(9)), qint64(0));
        // A whole video fits the parts a file may have.
        QVERIFY(attachmentPartCount(AttachmentLimits::maxVideoBytes) <= AttachmentLimits::maxParts);
        QVERIFY(attachmentPartCount(AttachmentLimits::maxAudioBytes) <= AttachmentLimits::maxParts);
    }

    void fileNamesArePathsNowhere()
    {
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("../../etc/passwd")),
                 QStringLiteral("_.._etc_passwd"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("C:\\Windows\\evil.exe")),
                 QStringLiteral("C__Windows_evil.exe"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("what? <really> \"yes\" | no*.txt")),
                 QStringLiteral("what_ _really_ _yes_ _ no_.txt"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("notes.txt:hidden")),
                 QStringLiteral("notes.txt_hidden"));
    }

    void fileNamesHideNothing()
    {
        // A right-to-left override would show "invoiceexe.txt".
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("invoice\u202Etxt.exe")),
                 QStringLiteral("invoicetxt.exe"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("a\u2066b\u2069c\u200Ed\u061Ce")),
                 QStringLiteral("abcde"));
        // Zero-width and other format characters, a BOM, tag characters.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("pho\u200Bto\u200D\uFEFF.jpg")),
                 QStringLiteral("photo.jpg"));
        QCOMPARE(sanitizeAttachmentFileName(QString::fromUcs4(U"flag\U000E0067\U000E0062.png")),
                 QStringLiteral("flag.png"));
        // A line or paragraph separator would push ".exe" onto a clipped line.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("report.pdf\u2028.exe")),
                 QStringLiteral("report.pdf.exe"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("report.pdf\u2029.exe")),
                 QStringLiteral("report.pdf.exe"));
        // Controls go; the whitespace ones separate words rather than glue them.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("bell\u0007\u001B[31m.txt")),
                 QStringLiteral("bell[31m.txt"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("two\tlines\nhere.txt")),
                 QStringLiteral("two lines here.txt"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("  spaced \u00A0\u3000  out  .txt  ")),
                 QStringLiteral("spaced out .txt"));
        // Private-use, unassigned code points and lone surrogates.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("a\uE000b\u0378c")), QStringLiteral("abc"));
        QString lone = QStringLiteral("ab");
        lone.insert(1, QChar(0xD800));
        lone.append(QChar(0xDC00));
        QCOMPARE(sanitizeAttachmentFileName(lone), QStringLiteral("ab"));
    }

    void fileNamesAreComposedAfterCleaning()
    {
        // NFC, and composed across a character that was dropped.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("Cafe\u0301.txt")),
                 QStringLiteral("Caf\u00E9.txt"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("Cafe\u200B\u0301.txt")),
                 QStringLiteral("Caf\u00E9.txt"));
        // Emoji and scripts are kept.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("Урок 😀 日本.pdf")),
                 QStringLiteral("Урок 😀 日本.pdf"));
    }

    void fileNamesSurviveWindows()
    {
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("CON")), QStringLiteral("_CON"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("nul.txt")), QStringLiteral("_nul.txt"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("com1.tar.gz")), QStringLiteral("_com1.tar.gz"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("Lpt9 .log")), QStringLiteral("_Lpt9 .log"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("CONSOLE.txt")), QStringLiteral("CONSOLE.txt"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("LPT10")), QStringLiteral("LPT10"));
        // Trailing dots and spaces are dropped by Windows; leading dots hide files.
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("report.pdf... ")),
                 QStringLiteral("report.pdf"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral(" ...bashrc")), QStringLiteral("bashrc"));
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("...")), QString());
        QCOMPARE(sanitizeAttachmentFileName(QStringLiteral("\u202E\u200B")), QString());
        QCOMPARE(sanitizeAttachmentFileName(QString()), QString());
    }

    void longFileNamesKeepAShortExtension()
    {
        constexpr int limit = AttachmentLimits::maxFileNameLength;
        const QString longName = QString(300, u'a') + QStringLiteral(".pdf");
        QCOMPARE(sanitizeAttachmentFileName(longName), QString(limit - 4, u'a') + QStringLiteral(".pdf"));

        // An extension over maxExtensionLength is just more name.
        const QString longExtension = QString(300, u'a') + u'.' + QString(20, u'x');
        QCOMPARE(sanitizeAttachmentFileName(longExtension), QString(limit, u'a'));
        const QString sixteen = QString(300, u'a') + u'.' + QString(16, u'x');
        QCOMPARE(sanitizeAttachmentFileName(sixteen), QString(limit - 17, u'a') + u'.' + QString(16, u'x'));

        // The cut never splits a surrogate pair, and never leaves a dot or a
        // space at the end.
        const QString emoji = QString(limit - 1, u'b') + QStringLiteral("😀") + QString(10, u'c');
        QCOMPARE(sanitizeAttachmentFileName(emoji), QString(limit - 1, u'b'));
        const QString spaced = QString(limit - 1, u'd') + QStringLiteral(" e") + QString(10, u'f');
        QCOMPARE(sanitizeAttachmentFileName(spaced), QString(limit - 1, u'd'));
        const QString dotty = QString(100, u'g') + QString(40, u'.') + QStringLiteral(".txt");
        QCOMPARE(sanitizeAttachmentFileName(dotty), QString(100, u'g') + QStringLiteral(".txt"));
    }

    void cleaningIsIdempotent()
    {
        const QStringList names = {
            QStringLiteral("../../etc/passwd"),
            QStringLiteral("invoice\u202Etxt.exe"),
            QStringLiteral("Cafe\u200B\u0301.txt"),
            QStringLiteral("  spaced \u00A0\u3000  out  .txt  "),
            QStringLiteral("nul .txt"),
            QStringLiteral("x<\u0338y.txt"),
            QString(300, u'a') + QStringLiteral(".pdf"),
            QString(119, u'b') + QStringLiteral("😀") + QString(10, u'c'),
            QString(100, u'g') + QString(40, u'.') + QStringLiteral(".txt"),
            QStringLiteral("CON") + QString(200, u'.') + QStringLiteral(".txt"),
        };
        for (const QString &name : names) {
            const QString once = sanitizeAttachmentFileName(name);
            QCOMPARE(sanitizeAttachmentFileName(once), once);
            QVERIFY(once.size() <= AttachmentLimits::maxFileNameLength);
        }
    }

    void mimeTypesFollowTheTokenGrammar()
    {
        QVERIFY(isValidMimeType(QStringLiteral("image/jpeg")));
        QVERIFY(isValidMimeType(QStringLiteral("audio/x-wav")));
        QVERIFY(isValidMimeType(QStringLiteral("application/octet-stream")));
        QVERIFY(isValidMimeType(
            QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document")));
        QVERIFY(isValidMimeType(QStringLiteral("text/x-c++src")));
        QVERIFY(isValidMimeType(QStringLiteral("model/3mf")));

        QVERIFY(!isValidMimeType(QString()));
        QVERIFY(!isValidMimeType(QStringLiteral("image")));
        QVERIFY(!isValidMimeType(QStringLiteral("image/")));
        QVERIFY(!isValidMimeType(QStringLiteral("/jpeg")));
        QVERIFY(!isValidMimeType(QStringLiteral("image/jpeg; charset=binary")));
        QVERIFY(!isValidMimeType(QStringLiteral("text/html<img src=x>")));
        QVERIFY(!isValidMimeType(QStringLiteral("a/b/c")));
        QVERIFY(!isValidMimeType(QStringLiteral("-image/jpeg")));
        QVERIFY(!isValidMimeType(QStringLiteral("image/.jpeg")));
        QVERIFY(!isValidMimeType(QStringLiteral("imäge/jpeg")));
        QVERIFY(!isValidMimeType(QStringLiteral("image/jp eg")));
        QVERIFY(!isValidMimeType(QStringLiteral("application/") + QString(95, u'x')));
        QVERIFY(isValidMimeType(QStringLiteral("application/") + QString(88, u'x')));
    }

    void descriptorsAreHeldToTheirKindsBounds()
    {
        for (const auto kind : {AttachmentKind::Image, AttachmentKind::Video, AttachmentKind::Audio,
                                AttachmentKind::File})
            QVERIFY(isValidDescriptor(descriptorFor(kind)));

        const auto broken
            = [](AttachmentKind kind, const std::function<void(AttachmentDescriptor &)> &change) {
                  AttachmentDescriptor descriptor = descriptorFor(kind);
                  change(descriptor);
                  return !isValidDescriptor(descriptor);
              };
        using D = AttachmentDescriptor;
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.key.chop(1); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.key.clear(); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.sha256.append('x'); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.kind = AttachmentKind(5); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.kind = AttachmentKind(0); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) {
            d.byteCount = 0;
            d.partCount = 0;
        }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.partCount += 1; }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) {
            d.byteCount = AttachmentLimits::maxFileBytes + 1;
            d.partCount = attachmentPartCount(d.byteCount);
        }));
        QVERIFY(broken(AttachmentKind::Image, [](D &d) {
            d.byteCount = AttachmentLimits::maxImageBytes + 1;
            d.partCount = attachmentPartCount(d.byteCount);
        }));
        QVERIFY(broken(AttachmentKind::Audio, [](D &d) {
            d.byteCount = AttachmentLimits::maxAudioBytes + 1;
            d.partCount = attachmentPartCount(d.byteCount);
        }));
        QVERIFY(broken(AttachmentKind::Video, [](D &d) {
            d.byteCount = AttachmentLimits::maxVideoBytes + 1;
            d.partCount = attachmentPartCount(d.byteCount);
        }));
        QVERIFY(broken(AttachmentKind::Image,
                       [](D &d) { d.width = AttachmentLimits::maxImageDimension + 1; }));
        QVERIFY(broken(AttachmentKind::Video, [](D &d) { d.height = -1; }));
        QVERIFY(broken(AttachmentKind::Audio, [](D &d) { d.width = 10; }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.height = 10; }));
        QVERIFY(broken(AttachmentKind::Image, [](D &d) { d.durationMs = 1; }));
        QVERIFY(broken(AttachmentKind::Video,
                       [](D &d) { d.durationMs = AttachmentLimits::maxVideoMs + 1001; }));
        QVERIFY(broken(AttachmentKind::Audio,
                       [](D &d) { d.durationMs = AttachmentLimits::maxAudioMs + 1001; }));
        QVERIFY(broken(AttachmentKind::Audio, [](D &d) { d.durationMs = -1; }));
        QVERIFY(broken(AttachmentKind::Audio, [](D &d) { d.peaks.append('x'); }));
        QVERIFY(broken(AttachmentKind::Image, [](D &d) { d.peaks = "x"; }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.mimeType = QStringLiteral("text/html<b>"); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.fileName = QStringLiteral("../escape.pdf"); }));
        QVERIFY(broken(AttachmentKind::File, [](D &d) { d.fileName = QString(121, u'n'); }));

        // Within the slack an encoder's last frame needs.
        AttachmentDescriptor song = descriptorFor(AttachmentKind::Audio);
        song.durationMs = chatSongLimits().maxTotalSamples * 1000 / SongContainer::sampleRate;
        QVERIFY(isValidDescriptor(song));
        // A file may be nameless and typeless; the UI calls it "File".
        AttachmentDescriptor bare = descriptorFor(AttachmentKind::File);
        bare.fileName.clear();
        bare.mimeType.clear();
        QVERIFY(isValidDescriptor(bare));
    }

    void labelsStandInWhereOnlyTextFits()
    {
        QCOMPARE(attachmentLabel(AttachmentKind::Image, QStringLiteral("a.jpg")), QStringLiteral("Photo"));
        QCOMPARE(attachmentLabel(AttachmentKind::Video, QString()), QStringLiteral("Video"));
        QCOMPARE(attachmentLabel(AttachmentKind::Audio, QStringLiteral("x.m4a")), QStringLiteral("Audio"));
        QCOMPARE(attachmentLabel(AttachmentKind::File, QStringLiteral("Lease.pdf")),
                 QStringLiteral("Lease.pdf"));
        QCOMPARE(attachmentLabel(AttachmentKind::File, QString()), QStringLiteral("File"));

        QCOMPARE(attachmentSummary(AttachmentKind::Image, QString(), QString()), QStringLiteral("Photo"));
        QCOMPARE(attachmentSummary(AttachmentKind::Image, QString(), QStringLiteral("  \n ")),
                 QStringLiteral("Photo"));
        QCOMPARE(attachmentSummary(AttachmentKind::Image, QString(),
                                   QStringLiteral("Sunset\n\nover the bay ")),
                 QStringLiteral("Photo: Sunset over the bay"));
        QCOMPARE(attachmentSummary(AttachmentKind::File, QStringLiteral("Lease.pdf"),
                                   QStringLiteral("Signed")),
                 QStringLiteral("Lease.pdf: Signed"));
    }

    void frameHeadersAreBoundedBeforeAnythingIsOpened()
    {
        const AttachmentId id = AttachmentId::generate();
        const QByteArray header = attachmentFrameHeader(AttachmentFrameType::Part, id, 0x01020304);
        QCOMPARE(header.size(), attachmentFrameHeaderBytes);
        QCOMPARE(quint8(header[0]), quint8(0xAC));
        QCOMPARE(quint8(header[1]), quint8(1));
        QCOMPARE(quint8(header[2]), quint8(1));
        QCOMPARE(header.mid(3, 16), id.bytes());
        QCOMPARE(header.mid(19), QByteArray::fromHex("01020304"));

        const QByteArray body(40, 'b');
        const QByteArray part = attachmentFrameHeader(AttachmentFrameType::Part, id, 73) + body;
        const auto split = splitAttachmentFrame(part);
        QVERIFY(split.has_value());
        QCOMPARE(split->first.type, AttachmentFrameType::Part);
        QCOMPARE(split->first.attachmentId, id);
        QCOMPARE(split->first.index, quint32(73));
        QCOMPARE(split->second, body);

        for (const auto type : {AttachmentFrameType::Preview, AttachmentFrameType::Request,
                                AttachmentFrameType::Cancel}) {
            QVERIFY(splitAttachmentFrame(attachmentFrameHeader(type, id, 0) + body));
            QVERIFY(!splitAttachmentFrame(attachmentFrameHeader(type, id, 1) + body));
            // Its own nonce and a tag, at least.
            QVERIFY(splitAttachmentFrame(attachmentFrameHeader(type, id, 0)
                                         + QByteArray(AttachmentLimits::controlSealOverhead, 'b')));
            QVERIFY(!splitAttachmentFrame(attachmentFrameHeader(type, id, 0)
                                          + QByteArray(AttachmentLimits::controlSealOverhead - 1, 'b')));
        }
        // Past the last part a file may have.
        QVERIFY(!splitAttachmentFrame(attachmentFrameHeader(AttachmentFrameType::Part, id,
                                                            quint32(AttachmentLimits::maxParts)) + body));
        QVERIFY(!splitAttachmentFrame(attachmentFrameHeader(AttachmentFrameType::Part, id, 0xFFFFFFFF)
                                      + body));
        // Magic, version, type.
        QVERIFY(!splitAttachmentFrame(withByte(part, 0, 0xAD)));
        QVERIFY(!splitAttachmentFrame(withByte(part, 1, 2)));
        QVERIFY(!splitAttachmentFrame(withByte(part, 2, 0)));
        QVERIFY(!splitAttachmentFrame(withByte(part, 2, 5)));
        // An all-zero attachment id is no id.
        QByteArray zeroId = part;
        zeroId.replace(3, 16, QByteArray(16, '\0'));
        QVERIFY(!splitAttachmentFrame(zeroId));
        // Shorter than a tag, empty, or larger than any frame.
        QVERIFY(!splitAttachmentFrame(attachmentFrameHeader(AttachmentFrameType::Part, id, 0)
                                      + QByteArray(15, 'b')));
        QVERIFY(splitAttachmentFrame(attachmentFrameHeader(AttachmentFrameType::Part, id, 0)
                                     + QByteArray(16, 'b')));
        QVERIFY(!splitAttachmentFrame(QByteArray()));
        QVERIFY(!splitAttachmentFrame(header));
        QVERIFY(!splitAttachmentFrame(
            attachmentFrameHeader(AttachmentFrameType::Part, id, 0)
            + QByteArray(AttachmentLimits::maxFrameBytes - attachmentFrameHeaderBytes + 1, 'b')));
    }

    void sealedFramesOpenWithTheirKey()
    {
        const QByteArray key = randomAttachmentKey();
        QCOMPARE(key.size(), AttachmentLimits::keyBytes);
        const AttachmentId id = AttachmentId::generate();

        // The largest part fits a frame with room to spare.
        const QByteArray part(AttachmentLimits::partBytes, 'p');
        const QByteArray frame = sealAttachmentFrame(key, AttachmentFrameType::Part, id, 5, part);
        QCOMPARE(frame.size(), attachmentFrameHeaderBytes + part.size() + AttachmentLimits::sealOverhead);
        QVERIFY(frame.size() <= AttachmentLimits::maxFrameBytes);
        QCOMPARE(frame.first(attachmentFrameHeaderBytes),
                 attachmentFrameHeader(AttachmentFrameType::Part, id, 5));
        QVERIFY(!frame.contains(QByteArray(64, 'p'))); // no plaintext on the wire
        const auto split = splitAttachmentFrame(frame);
        QVERIFY(split.has_value());
        QCOMPARE(split->first.index, quint32(5));
        QCOMPARE(openAttachmentFrame(key, frame), std::optional<QByteArray>(part));
        // The file store keeps bodies apart from their headers.
        QCOMPARE(openAttachmentBody(key, attachmentFrameHeader(AttachmentFrameType::Part, id, 5),
                                    split->second),
                 std::optional<QByteArray>(part));

        const QByteArray preview = realJpeg(64, 48);
        const QByteArray previewFrame
            = sealAttachmentFrame(key, AttachmentFrameType::Preview, id, 0, preview);
        QCOMPARE(openAttachmentFrame(key, previewFrame), std::optional<QByteArray>(preview));
        const QByteArray request = sealAttachmentFrame(key, AttachmentFrameType::Request, id, 0,
                                                       QByteArray::fromHex("ff03"));
        QCOMPARE(openAttachmentFrame(key, request), std::optional<QByteArray>(QByteArray::fromHex("ff03")));
        // A cancel carries nothing, but is still authenticated.
        const QByteArray cancel = sealAttachmentFrame(key, AttachmentFrameType::Cancel, id, 0, {});
        QCOMPARE(cancel.size(), attachmentFrameHeaderBytes + AttachmentLimits::controlSealOverhead);
        const auto opened = openAttachmentFrame(key, cancel);
        QVERIFY(opened.has_value());
        QVERIFY(opened->isEmpty());

        // Sealing is deterministic for one key, type and index (a part sent
        // again is the same bytes), and differs with any of them.
        QCOMPARE(sealAttachmentFrame(key, AttachmentFrameType::Part, id, 5, part), frame);
        const QByteArray other = sealAttachmentFrame(key, AttachmentFrameType::Part, id, 6, part);
        QVERIFY(other.sliced(attachmentFrameHeaderBytes) != frame.sliced(attachmentFrameHeaderBytes));
    }

    void requestsNeverShareANonce()
    {
        // A receiver asks again for fewer parts, and every member of a group
        // asks under the same key: each request must get a nonce of its own,
        // or the relay could learn the keystream and forge frames.
        const QByteArray key = randomAttachmentKey();
        const AttachmentId id = AttachmentId::generate();
        const QByteArray firstBitmap = QByteArray::fromHex("ffffffffffffffffff03");
        const QByteArray secondBitmap = QByteArray::fromHex("0fffffffffffffffff03");
        const QByteArray first = sealAttachmentFrame(key, AttachmentFrameType::Request, id, 0, firstBitmap);
        const QByteArray second = sealAttachmentFrame(key, AttachmentFrameType::Request, id, 0, secondBitmap);
        const QByteArray again = sealAttachmentFrame(key, AttachmentFrameType::Request, id, 0, firstBitmap);
        for (const QByteArray &frame : {first, second, again}) {
            QCOMPARE(frame.size(), attachmentFrameHeaderBytes + firstBitmap.size()
                                       + AttachmentLimits::controlSealOverhead);
            // Top bit set: never one of the Part nonces, which start 00.
            QVERIFY(quint8(frame[attachmentFrameHeaderBytes]) & 0x80);
        }
        const auto nonceOf = [](const QByteArray &frame) {
            return frame.mid(attachmentFrameHeaderBytes, AttachmentLimits::controlNonceBytes);
        };
        QVERIFY(nonceOf(first) != nonceOf(second));
        QVERIFY(nonceOf(first) != nonceOf(again));
        QVERIFY(first != again); // the same bitmap sealed twice is not the same bytes
        // Their ciphertexts do not differ by what their plaintexts differ by.
        const auto ciphertextOf = [&](const QByteArray &frame) {
            return frame.mid(attachmentFrameHeaderBytes + AttachmentLimits::controlNonceBytes,
                             firstBitmap.size());
        };
        QByteArray cipherXor = ciphertextOf(first);
        QByteArray plainXor = firstBitmap;
        for (qsizetype i = 0; i < cipherXor.size(); ++i) {
            cipherXor[i] = char(cipherXor[i] ^ ciphertextOf(second)[i]);
            plainXor[i] = char(plainXor[i] ^ secondBitmap[i]);
        }
        QVERIFY(cipherXor != plainXor);
        QCOMPARE(openAttachmentFrame(key, first), std::optional<QByteArray>(firstBitmap));
        QCOMPARE(openAttachmentFrame(key, second), std::optional<QByteArray>(secondBitmap));
        QCOMPARE(openAttachmentFrame(key, again), std::optional<QByteArray>(firstBitmap));

        // Previews and cancels draw their own too.
        const QByteArray preview = realJpeg(32, 24);
        QVERIFY(nonceOf(sealAttachmentFrame(key, AttachmentFrameType::Preview, id, 0, preview))
                != nonceOf(sealAttachmentFrame(key, AttachmentFrameType::Preview, id, 0, preview)));
        QVERIFY(nonceOf(sealAttachmentFrame(key, AttachmentFrameType::Cancel, id, 0, {}))
                != nonceOf(sealAttachmentFrame(key, AttachmentFrameType::Cancel, id, 0, {})));
    }

    void tamperedFramesNeverOpen()
    {
        const QByteArray key = randomAttachmentKey();
        const AttachmentId id = AttachmentId::generate();
        const QByteArray plaintext("the quick brown fox jumps over the lazy dog");
        const QByteArray frame = sealAttachmentFrame(key, AttachmentFrameType::Part, id, 3, plaintext);
        QVERIFY(openAttachmentFrame(key, frame).has_value());

        // The header is associated data: its id, type and index are bound.
        QVERIFY(!openAttachmentFrame(key, flipped(frame, 5)));
        QVERIFY(!openAttachmentFrame(key, withByte(frame, 22, 4)));
        QVERIFY(!openAttachmentFrame(key, withByte(frame, 2, quint8(AttachmentFrameType::Preview))));
        // Body and tag.
        QVERIFY(!openAttachmentFrame(key, flipped(frame, attachmentFrameHeaderBytes)));
        QVERIFY(!openAttachmentFrame(key, flipped(frame, frame.size() - 1)));
        QVERIFY(!openAttachmentFrame(key, frame.chopped(1)));
        QVERIFY(!openAttachmentFrame(key, frame + 'x'));
        // Another key, a key of the wrong size.
        QVERIFY(!openAttachmentFrame(randomAttachmentKey(), frame));
        QVERIFY(!openAttachmentFrame(key.left(16), frame));
        // A body moved under another part's header.
        const QByteArray moved = attachmentFrameHeader(AttachmentFrameType::Part, id, 4)
            + frame.sliced(attachmentFrameHeaderBytes);
        QVERIFY(!openAttachmentFrame(key, moved));
        QVERIFY(!openAttachmentBody(
            key, attachmentFrameHeader(AttachmentFrameType::Part, AttachmentId::generate(), 3),
            frame.sliced(attachmentFrameHeaderBytes)));
        QVERIFY(!openAttachmentBody(key, QByteArray(22, 'h'), frame.sliced(attachmentFrameHeaderBytes)));
        QVERIFY(!openAttachmentBody(key, frame.first(attachmentFrameHeaderBytes), QByteArray(15, 't')));

        // A frame that carries its nonce: the nonce is bound too, and must
        // keep its top bit.
        const QByteArray request = sealAttachmentFrame(key, AttachmentFrameType::Request, id, 0,
                                                       QByteArray::fromHex("ff03"));
        QVERIFY(openAttachmentFrame(key, request).has_value());
        QVERIFY(!openAttachmentFrame(key, flipped(request, attachmentFrameHeaderBytes + 5)));
        QVERIFY(!openAttachmentFrame(key, withByte(request, attachmentFrameHeaderBytes,
                                                   quint8(request[attachmentFrameHeaderBytes]) & 0x7F)));
        QVERIFY(!openAttachmentFrame(key, flipped(request, request.size() - 1)));
        QVERIFY(!openAttachmentFrame(key, withByte(request, 2, quint8(AttachmentFrameType::Preview))));
        QVERIFY(!openAttachmentFrame(key, withByte(request, 2, quint8(AttachmentFrameType::Cancel))));
        QVERIFY(!openAttachmentBody(key, request.first(attachmentFrameHeaderBytes),
                                    request.sliced(attachmentFrameHeaderBytes).first(27)));
        // A Part's body cannot pass for a Request's or the other way round.
        QVERIFY(!openAttachmentBody(key, attachmentFrameHeader(AttachmentFrameType::Part, id, 0),
                                    request.sliced(attachmentFrameHeaderBytes)));
        QVERIFY(!openAttachmentBody(key, attachmentFrameHeader(AttachmentFrameType::Request, id, 0),
                                    frame.sliced(attachmentFrameHeaderBytes)));
    }

    void sealingRefusesWhatNoReceiverWouldTake()
    {
        const QByteArray key = randomAttachmentKey();
        const AttachmentId id = AttachmentId::generate();
        QVERIFY(sealAttachmentFrame(key.left(31), AttachmentFrameType::Part, id, 0, "x").isEmpty());
        QVERIFY(sealAttachmentFrame({}, AttachmentFrameType::Part, id, 0, "x").isEmpty());
        QVERIFY(sealAttachmentFrame(key, AttachmentFrameType::Part, id, AttachmentLimits::maxParts, "x")
                    .isEmpty());
        QVERIFY(sealAttachmentFrame(key, AttachmentFrameType::Cancel, id, 1, {}).isEmpty());
        QVERIFY(sealAttachmentFrame(key, AttachmentFrameType(7), id, 0, "x").isEmpty());
        const qsizetype room = AttachmentLimits::maxFrameBytes - attachmentFrameHeaderBytes
            - AttachmentLimits::sealOverhead;
        QVERIFY(!sealAttachmentFrame(key, AttachmentFrameType::Part, id, 0, QByteArray(room, 'r'))
                     .isEmpty());
        QVERIFY(sealAttachmentFrame(key, AttachmentFrameType::Part, id, 0, QByteArray(room + 1, 'r'))
                    .isEmpty());
        // The others also carry their nonce.
        const qsizetype controlRoom = room - AttachmentLimits::controlNonceBytes;
        QVERIFY(!sealAttachmentFrame(key, AttachmentFrameType::Preview, id, 0,
                                     QByteArray(controlRoom, 'r')).isEmpty());
        QVERIFY(sealAttachmentFrame(key, AttachmentFrameType::Preview, id, 0,
                                    QByteArray(controlRoom + 1, 'r')).isEmpty());
    }

    void randomKeysAreFresh()
    {
        const QByteArray first = randomAttachmentKey();
        const QByteArray second = randomAttachmentKey();
        QCOMPARE(first.size(), AttachmentLimits::keyBytes);
        QVERIFY(first != second);
        QVERIFY(first != QByteArray(AttachmentLimits::keyBytes, '\0'));
    }

    void videoSequencesRoundTripAndStayUniform()
    {
        const QVector<QByteArray> segments = {clipSegment(), clipSegment(64, 48, 15, 'w'),
                                              clipSegment(64, 48, 15, 'x')};
        for (const QByteArray &segment : segments)
            QVERIFY(!segment.isEmpty());
        const QByteArray blob = encodeVideoSequence(segments);
        QVERIFY(blob.startsWith("OCVS"));
        QCOMPARE(decodeVideoSequence(blob), std::optional<QVector<QByteArray>>(segments));

        QVERIFY(encodeVideoSequence({}).isEmpty());
        QVERIFY(encodeVideoSequence({clipSegment(), clipSegment(80, 48)}).isEmpty());
        QVERIFY(encodeVideoSequence({clipSegment(), clipSegment(64, 48, 10)}).isEmpty());
        QVERIFY(encodeVideoSequence({clipSegment(), QByteArray("not a clip")}).isEmpty());
        QVector<QByteArray> tooMany(AttachmentLimits::maxVideoSegments + 1, clipSegment());
        QVERIFY(encodeVideoSequence(tooMany).isEmpty());
        tooMany.removeLast();
        QVERIFY(!encodeVideoSequence(tooMany).isEmpty());

        // Anything appended, another magic or version, a segment not a clip.
        QVERIFY(!decodeVideoSequence(blob + 'x'));
        QVERIFY(!decodeVideoSequence(withByte(blob, 0, 'X')));
        QVERIFY(!decodeVideoSequence(QByteArray("OCVS")));
        const auto sequence = [](const QCborValue &version, const QCborArray &list) {
            return QByteArray("OCVS") + QCborValue(QCborArray{version, list}).toCbor();
        };
        QVERIFY(decodeVideoSequence(sequence(1, QCborArray{clipSegment()})));
        QVERIFY(!decodeVideoSequence(sequence(2, QCborArray{clipSegment()})));
        QVERIFY(!decodeVideoSequence(sequence(1, QCborArray{})));
        QVERIFY(!decodeVideoSequence(sequence(1, QCborArray{clipSegment(), QStringLiteral("text")})));
        QVERIFY(!decodeVideoSequence(sequence(1, QCborArray{clipSegment(), QByteArray("OCCL junk")})));
        QVERIFY(!decodeVideoSequence(sequence(1, QCborArray{clipSegment(), clipSegment(96, 48)})));
    }

    void imagesMustBeTheJpegTheyWereDescribedAs()
    {
        const QByteArray jpeg = realJpeg(300, 200);
        QVERIFY(!jpeg.isEmpty());
        AttachmentDescriptor descriptor = descriptorFor(AttachmentKind::Image, jpeg.size());
        descriptor.sha256 = sha256(jpeg);
        descriptor.width = 300;
        descriptor.height = 200;
        QVERIFY(attachmentBlobIsValid(descriptor, jpeg));

        // Progressive pictures are fine within the scan cap.
        const QByteArray progressive = realJpeg(300, 200, true);
        QVERIFY(jpegScanCount(progressive) > 1);
        AttachmentDescriptor progressiveDescriptor = descriptor;
        progressiveDescriptor.byteCount = progressive.size();
        progressiveDescriptor.partCount = 1;
        progressiveDescriptor.sha256 = sha256(progressive);
        QVERIFY(attachmentBlobIsValid(progressiveDescriptor, progressive));

        // The bytes the hash names, and no others.
        QVERIFY(!attachmentBlobIsValid(descriptor, flipped(jpeg, jpeg.size() / 2)));
        QVERIFY(!attachmentBlobIsValid(descriptor, jpeg.chopped(1)));
        AttachmentDescriptor wrongHash = descriptor;
        wrongHash.sha256 = sha256("other");
        QVERIFY(!attachmentBlobIsValid(wrongHash, jpeg));
        // A frame size other than described would break the bubble's layout.
        AttachmentDescriptor wrongSize = descriptor;
        wrongSize.width = 301;
        QVERIFY(!attachmentBlobIsValid(wrongSize, jpeg));
        // An undescribed size is taken as it comes.
        AttachmentDescriptor unknownSize = descriptor;
        unknownSize.width = 0;
        unknownSize.height = 0;
        QVERIFY(attachmentBlobIsValid(unknownSize, jpeg));

        // A scan bomb, a picture larger than accepted, something else entirely.
        const auto describing = [&](const QByteArray &blob, int width, int height) {
            AttachmentDescriptor d = descriptorFor(AttachmentKind::Image, blob.size());
            d.sha256 = sha256(blob);
            d.width = width;
            d.height = height;
            return d;
        };
        const QByteArray bomb = structuralJpeg(200, 100, maxJpegScans + 1);
        QVERIFY(!attachmentBlobIsValid(describing(bomb, 200, 100), bomb));
        const QByteArray manyScans = structuralJpeg(200, 100, maxJpegScans);
        QVERIFY(attachmentBlobIsValid(describing(manyScans, 200, 100), manyScans));
        const QByteArray huge = structuralJpeg(AttachmentLimits::maxImageDimension + 1, 100, 1);
        QVERIFY(!attachmentBlobIsValid(describing(huge, 0, 0), huge));
        // A frame the decoder holds whole is taken only as large as this
        // version sends: an 8192 px progressive picture costs ~400 MiB to
        // draw even as a thumbnail. A single-scan baseline one streams.
        const int side = AttachmentLimits::maxBufferedImageSide;
        const QByteArray largeBaseline = structuralJpeg(AttachmentLimits::maxImageDimension, 100, 1, 0xC0);
        QVERIFY(attachmentBlobIsValid(describing(largeBaseline, 0, 0), largeBaseline));
        for (const quint8 marker : {quint8(0xC2), quint8(0xC3), quint8(0xC9), quint8(0xCA)}) {
            const QByteArray fits = structuralJpeg(side, 100, 1, marker);
            QVERIFY(attachmentBlobIsValid(describing(fits, 0, 0), fits));
            const QByteArray tooLarge = structuralJpeg(100, side + 1, 1, marker);
            QVERIFY(!attachmentBlobIsValid(describing(tooLarge, 0, 0), tooLarge));
        }
        // A baseline frame coded as a scan per component is held whole too.
        const QByteArray multiScan = structuralJpeg(side + 1, 100, 3, 0xC0);
        QVERIFY(!attachmentBlobIsValid(describing(multiScan, 0, 0), multiScan));
        const QByteArray png("\x89PNG\r\n\x1a\n rest", 14);
        QVERIFY(!attachmentBlobIsValid(describing(png, 0, 0), png));
    }

    void videosAudioAndFilesAreCheckedByKind()
    {
        const QByteArray video = encodeVideoSequence({clipSegment(), clipSegment()});
        AttachmentDescriptor videoDescriptor = descriptorFor(AttachmentKind::Video, video.size());
        videoDescriptor.sha256 = sha256(video);
        QVERIFY(attachmentBlobIsValid(videoDescriptor, video));
        const QByteArray notVideo(video.size(), 'z');
        videoDescriptor.sha256 = sha256(notVideo);
        QVERIFY(!attachmentBlobIsValid(videoDescriptor, notVideo));

        const QByteArray song = encodeSongContainer(songOf(200), chatSongLimits());
        QVERIFY(!song.isEmpty());
        AttachmentDescriptor audioDescriptor = descriptorFor(AttachmentKind::Audio, song.size());
        audioDescriptor.sha256 = sha256(song);
        QVERIFY(attachmentBlobIsValid(audioDescriptor, song));
        const QByteArray notSong = QByteArray("OCSG") + QByteArray(song.size() - 4, '\x01');
        audioDescriptor.sha256 = sha256(notSong);
        QVERIFY(!attachmentBlobIsValid(audioDescriptor, notSong));

        // A file is whatever it is, once its size and hash match.
        const QByteArray file(AttachmentLimits::partBytes + 3, '\x7F');
        AttachmentDescriptor fileDescriptor = descriptorFor(AttachmentKind::File, file.size());
        fileDescriptor.sha256 = sha256(file);
        QVERIFY(attachmentBlobIsValid(fileDescriptor, file));
        QVERIFY(!attachmentBlobIsValid(fileDescriptor, flipped(file, 0)));
        // Nothing is checked against a descriptor that is itself invalid.
        fileDescriptor.key.clear();
        QVERIFY(!attachmentBlobIsValid(fileDescriptor, file));
    }

    void previewsAreSmallPicturesOnly()
    {
        QVERIFY(previewIsAcceptable(realJpeg(320, 240, false, 60)));
        QVERIFY(previewIsAcceptable(realJpeg(180, 320, false, 60)));
        QVERIFY(previewIsAcceptable(realJpeg(40, 30, true, 60)));
        QVERIFY(!previewIsAcceptable(realJpeg(321, 100, false, 30)));
        QVERIFY(!previewIsAcceptable(realJpeg(100, 321, false, 30)));
        // Within the side but over the byte cap.
        const QByteArray heavy = realJpeg(320, 320, false, 100, true);
        QVERIFY(heavy.size() > AttachmentLimits::maxPreviewBytes);
        QVERIFY(!previewIsAcceptable(heavy));
        QVERIFY(!previewIsAcceptable(structuralJpeg(100, 100, maxJpegScans + 1)));
        QVERIFY(previewIsAcceptable(structuralJpeg(100, 100, 3)));
        QVERIFY(!previewIsAcceptable(QByteArray()));
        QVERIFY(!previewIsAcceptable(QByteArray("\xFF\xD8\xFF", 3)));
    }

    void frameSizeComesFromTheFirstFrameHeader()
    {
        QCOMPARE(jpegFrameSize(realJpeg(123, 45)), std::optional(std::pair{123, 45}));
        QCOMPARE(jpegFrameSize(realJpeg(64, 640, true)), std::optional(std::pair{64, 640}));
        QCOMPARE(jpegFrameSize(structuralJpeg(8192, 2, 1, 0xC0)), std::optional(std::pair{8192, 2}));
        QCOMPARE(jpegFrameSize(structuralJpeg(10, 20, 1, 0xC1)), std::optional(std::pair{10, 20}));
        QCOMPARE(jpegFrame(structuralJpeg(10, 20, 1, 0xC2))->marker, quint8(0xC2));
        // Which frames stream through the decoder.
        QVERIFY(!jpegIsBuffered(realJpeg(123, 45)));
        QVERIFY(jpegIsBuffered(realJpeg(123, 45, true)));
        QVERIFY(!jpegIsBuffered(structuralJpeg(10, 20, 1, 0xC0)));
        QVERIFY(!jpegIsBuffered(structuralJpeg(10, 20, 1, 0xC1)));
        QVERIFY(jpegIsBuffered(structuralJpeg(10, 20, 1, 0xC2)));
        QVERIFY(jpegIsBuffered(structuralJpeg(10, 20, 2, 0xC0)));
        QVERIFY(jpegIsBuffered(QByteArray("GIF89a")));
        // DHT shares the marker range but is no frame header.
        QVERIFY(!jpegFrameSize(structuralJpeg(10, 20, 1, 0xC4)));
        // Height deferred to a DNL marker, a zero width.
        QVERIFY(!jpegFrameSize(structuralJpeg(10, 0, 1)));
        QVERIFY(!jpegFrameSize(structuralJpeg(0, 10, 1)));
        // Truncated inside the frame header, or no JPEG at all.
        const QByteArray jpeg = structuralJpeg(10, 20, 1);
        const qsizetype frameAt = jpeg.indexOf(QByteArray("\xFF\xC2", 2));
        QVERIFY(frameAt > 0);
        QVERIFY(!jpegFrameSize(jpeg.first(frameAt + 6)));
        QVERIFY(!jpegFrameSize(QByteArray("GIF89a")));
        QVERIFY(!jpegFrameSize(QByteArray()));
    }

    void fiveMinuteSongsFitChatLimitsOnly()
    {
        const SongContainerLimits chat = chatSongLimits();
        QCOMPARE(chat.maxBytes, qsizetype(AttachmentLimits::maxAudioBytes));
        QCOMPARE(chat.maxDurationMs, AttachmentLimits::maxAudioMs);
        QCOMPARE(chat.maxTotalSamples, qint64(300 * 48'000 + 24'000));
        // A profile song's limits are what they always were.
        const SongContainerLimits profile;
        QCOMPARE(profile.maxBytes, maxSongBytes);
        QCOMPARE(profile.maxTotalSamples, SongContainer::maxTotalSamples);
        QCOMPARE(qint64(profile.maxDurationMs), qint64(SongContainer::maxDurationMs));

        const SongContainer song = songOf(300);
        QCOMPARE(song.durationMs(), qint64(300'000));
        QVERIFY(encodeSongContainer(song).isEmpty());
        const QByteArray bytes = encodeSongContainer(song, chat);
        QVERIFY(!bytes.isEmpty());
        QVERIFY(bytes.size() > maxSongBytes);
        QVERIFY(bytes.size() <= chat.maxBytes);
        QVERIFY(!decodeSongContainer(bytes));
        const auto decoded = decodeSongContainer(bytes, chat);
        QVERIFY(decoded.has_value());
        QCOMPARE(*decoded, song);
        // Seeking to 4:30 lands inside the container's packets.
        const qint64 at = 270 * qint64(SongContainer::sampleRate);
        QVERIFY((at + decoded->preSkip) / decoded->frameSamples < decoded->packets.size());

        // Past five minutes and the encoder's slack, or past 4 MiB.
        SongContainer longer = songOf(301);
        QVERIFY(encodeSongContainer(longer, chat).isEmpty());
        const QByteArray tooBig = encodeSongContainer(songOf(290, 900), chat);
        QVERIFY(!tooBig.isEmpty()); // the encoder leaves the byte budget to its caller
        QVERIFY(tooBig.size() > chat.maxBytes);
        QVERIFY(!decodeSongContainer(tooBig, chat));

        // A profile-length song encodes to the same bytes either way.
        const SongContainer shortSong = songOf(30);
        QCOMPARE(encodeSongContainer(shortSong, chat), encodeSongContainer(shortSong));
        QCOMPARE(decodeSongContainer(encodeSongContainer(shortSong)), std::optional(shortSong));
    }
};

QTEST_GUILESS_MAIN(AttachmentTest)
#include "tst_attachment.moc"
