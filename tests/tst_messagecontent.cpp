#include <QtTest>

#include "domain/MessageContent.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCryptographicHash>

#include <functional>

using namespace OpenChat;

namespace {

// A tagged payload built by hand, for shapes the encoder never produces.
QByteArray tagged(const QCborArray &fields)
{
    QByteArray bytes(1, '\xFF');
    bytes.append(QCborValue(fields).toCbor());
    return bytes;
}

// A photo as the sender describes it.
AttachmentDescriptor photo()
{
    AttachmentDescriptor descriptor;
    descriptor.key = QByteArray(AttachmentLimits::keyBytes, 'k');
    descriptor.kind = AttachmentKind::Image;
    descriptor.byteCount = 1'234'567;
    descriptor.sha256 = QByteArray(32, 's');
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    descriptor.mimeType = QStringLiteral("image/jpeg");
    descriptor.fileName = QStringLiteral("Harbour at dusk.jpg");
    descriptor.width = 2048;
    descriptor.height = 1365;
    descriptor.hasPreview = true;
    return descriptor;
}

AttachmentDescriptor song()
{
    AttachmentDescriptor descriptor;
    descriptor.key = QByteArray(AttachmentLimits::keyBytes, 'K');
    descriptor.kind = AttachmentKind::Audio;
    descriptor.byteCount = 3'000'000;
    descriptor.sha256 = QByteArray(32, 'S');
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    descriptor.fileName = QStringLiteral("Rehearsal take 3.m4a");
    descriptor.durationMs = 287'500;
    for (int bar = 0; bar < AttachmentLimits::maxPeaks; ++bar)
        descriptor.peaks.append(char(bar * 2));
    return descriptor;
}

// The descriptor map as the encoder writes it, for payloads edited by hand.
QCborMap descriptorMap(const AttachmentDescriptor &descriptor)
{
    QCborMap map;
    map.insert(1, qint64(descriptor.kind));
    map.insert(2, descriptor.byteCount);
    map.insert(3, descriptor.sha256);
    map.insert(4, descriptor.partCount);
    map.insert(5, descriptor.key);
    if (!descriptor.mimeType.isEmpty())
        map.insert(6, descriptor.mimeType);
    if (!descriptor.fileName.isEmpty())
        map.insert(7, descriptor.fileName);
    if (descriptor.width)
        map.insert(8, descriptor.width);
    if (descriptor.height)
        map.insert(9, descriptor.height);
    if (descriptor.durationMs)
        map.insert(10, descriptor.durationMs);
    if (descriptor.hasPreview)
        map.insert(11, true);
    if (!descriptor.peaks.isEmpty())
        map.insert(12, descriptor.peaks);
    return map;
}

QByteArray attachmentPayload(const QCborValue &caption, const QCborValue &attachmentId,
                             const QCborValue &map, const QCborValue &reply = QCborValue(nullptr))
{
    return tagged({qint64(1), qint64(3), caption, attachmentId, map, reply});
}

QByteArray attachmentPayload(const AttachmentDescriptor &descriptor, const QCborMap &map)
{
    return attachmentPayload(QStringLiteral("caption"), descriptor.attachmentId.bytes(), map);
}

// What 0.2.8 and 0.2.9 do with a tagged payload, copied from them: only a
// reply or an edit is ever shown.
bool legacyClientShows(QByteArrayView bytes)
{
    if (bytes.isEmpty() || bytes.front() != '\xFF')
        return true;
    if (bytes.size() > 3 * (65536 + 200) + 1024)
        return false;
    QCborParserError error;
    const QCborValue root = QCborValue::fromCbor(bytes.sliced(1).toByteArray(), &error);
    if (error.error != QCborError::NoError || !root.isArray())
        return false;
    const QCborArray fields = root.toArray();
    if (fields.size() < 4 || !fields.at(0).isInteger() || fields.at(0).toInteger() != 1
        || !fields.at(1).isInteger() || !fields.at(2).isString() || !fields.at(3).isByteArray())
        return false;
    const qint64 type = fields.at(1).toInteger();
    if (!MessageId::fromBytes(fields.at(3).toByteArray()) || fields.at(2).toString().trimmed().isEmpty())
        return false;
    return type == 1 || type == 2;
}

} // namespace

// The conversation message codec: plain text stays the bare UTF-8 every
// earlier version reads, replies and edits round-trip, and anything tagged but
// malformed is refused rather than shown.
class MessageContentTest final : public QObject
{
    Q_OBJECT

private slots:
    void plainTextIsItsBareUtf8()
    {
        const QString text = QStringLiteral("Grüße, wie geht's? 👋");
        const QByteArray bytes = encodeMessageContent(MessageContent::text(text));
        QCOMPARE(bytes, text.toUtf8());
        const auto decoded = decodeMessageContent(bytes);
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->type, MessageContent::Type::Text);
        QCOMPARE(decoded->body, text);
        QVERIFY(!decoded->target);

        // Whatever an older client sent, however odd, is still text.
        const auto odd = decodeMessageContent(QByteArray("\x01\x02 {\"not\": cbor}"));
        QVERIFY(odd.has_value());
        QCOMPARE(odd->type, MessageContent::Type::Text);
    }

    void replyRoundTripsWithItsQuote()
    {
        const MessageId target = MessageId::generate();
        const DeviceId author = DeviceId::generate();
        const auto reply = MessageContent::reply(QStringLiteral("Yes, Saturday works"), target,
                                                 author, QStringLiteral("Are you free this weekend?"));
        const QByteArray bytes = encodeMessageContent(reply);
        QCOMPARE(bytes.front(), '\xFF');
        const auto decoded = decodeMessageContent(bytes);
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == reply);
        QCOMPARE(decoded->type, MessageContent::Type::Reply);
        QCOMPARE(*decoded->target, target);
        QCOMPARE(*decoded->quotedSender, author);
        QCOMPARE(decoded->quotedBody, QStringLiteral("Are you free this weekend?"));
    }

    void editRoundTrips()
    {
        const MessageId target = MessageId::generate();
        const auto edit = MessageContent::edit(target, QStringLiteral("See you at 8, not 7"));
        const auto decoded = decodeMessageContent(encodeMessageContent(edit));
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == edit);
        QCOMPARE(decoded->type, MessageContent::Type::Edit);
        QCOMPARE(*decoded->target, target);
        QVERIFY(!decoded->quotedSender);
    }

    void quoteIsCappedWithoutSplittingACharacter()
    {
        QString long_(maxQuoteLength - 1, QLatin1Char('a'));
        long_ += QStringLiteral("😀 and more");
        const QString excerpt = quoteExcerpt(long_);
        // The emoji would straddle the limit, so it is left out whole.
        QCOMPARE(excerpt, QString(maxQuoteLength - 1, QLatin1Char('a')));
        QCOMPARE(quoteExcerpt(QStringLiteral("short")), QStringLiteral("short"));

        const auto reply = MessageContent::reply(QStringLiteral("ok"), MessageId::generate(),
                                                 DeviceId::generate(), QString(500, QLatin1Char('q')));
        QCOMPARE(reply.quotedBody.size(), maxQuoteLength);
        const auto decoded = decodeMessageContent(encodeMessageContent(reply));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->quotedBody.size(), maxQuoteLength);
    }

    void refusesMalformedTaggedPayloads()
    {
        const QByteArray id = MessageId::generate().bytes();
        const QByteArray device = DeviceId::generate().bytes();

        // Not CBOR, not an array, too short.
        QVERIFY(!decodeMessageContent(QByteArray("\xFF\xFF\xFF", 3)));
        QVERIFY(!decodeMessageContent(QByteArray(1, '\xFF')));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(2), QStringLiteral("x")})));
        // A later version, an unknown type.
        QVERIFY(!decodeMessageContent(tagged({qint64(2), qint64(2), QStringLiteral("x"), id})));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(9), QStringLiteral("x"), id})));
        // A target that is not an id, an empty body.
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(2), QStringLiteral("x"),
                                              QByteArray(15, 'a')})));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(2), QStringLiteral("x"),
                                              QByteArray(16, '\0')})));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(2), QStringLiteral("  "), id})));
        // A reply without its quote, or with an oversized one.
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(1), QStringLiteral("x"), id})));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(1), QStringLiteral("x"), id,
                                              device, QString(maxQuoteLength + 1, QLatin1Char('q'))})));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(1), QStringLiteral("x"), id,
                                              QByteArray(3, 'd'), QStringLiteral("q")})));

        // Fields a later version appends are skipped, not refused.
        const auto extended = decodeMessageContent(
            tagged({qint64(1), qint64(2), QStringLiteral("x"), id, QStringLiteral("later")}));
        QVERIFY(extended.has_value());
        QCOMPARE(extended->body, QStringLiteral("x"));
    }

    void attachmentRoundTripsWithEveryField()
    {
        const auto message
            = MessageContent::attachmentMessage(QStringLiteral("The harbour at dusk 🌅"), photo());
        QCOMPARE(message.type, MessageContent::Type::Attachment);
        const QByteArray bytes = encodeMessageContent(message);
        QCOMPARE(bytes.front(), '\xFF');
        // Tiny: ids, sizes, a hash, a key and the caption; never a preview.
        QVERIFY(bytes.size() < 300);
        const auto decoded = decodeMessageContent(bytes);
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == message);
        QCOMPARE(decoded->type, MessageContent::Type::Attachment);
        QCOMPARE(decoded->body, QStringLiteral("The harbour at dusk 🌅"));
        QCOMPARE(*decoded->attachment, *message.attachment);
        QCOMPARE(decoded->attachment->width, 2048);
        QVERIFY(!decoded->target);
        QVERIFY(!decoded->quotedSender);

        const auto audio = MessageContent::attachmentMessage(QString(), song());
        const auto decodedAudio = decodeMessageContent(encodeMessageContent(audio));
        QVERIFY(decodedAudio.has_value());
        QCOMPARE(*decodedAudio->attachment, *audio.attachment);
        QCOMPARE(decodedAudio->attachment->peaks.size(), AttachmentLimits::maxPeaks);
    }

    void attachmentCaptionMayBeEmpty()
    {
        const auto bare = MessageContent::attachmentMessage(QString(), photo());
        const auto decoded = decodeMessageContent(encodeMessageContent(bare));
        QVERIFY(decoded.has_value());
        QVERIFY(decoded->body.isEmpty());
        QVERIFY(*decoded == bare);

        // Whitespace alone is no caption, on either side.
        QVERIFY(MessageContent::attachmentMessage(QStringLiteral(" \n\t "), photo()).body.isEmpty());
        const auto spaces = decodeMessageContent(attachmentPayload(
            QStringLiteral("  \n "), photo().attachmentId.bytes(), descriptorMap(photo())));
        QVERIFY(spaces.has_value());
        QVERIFY(spaces->body.isEmpty());
    }

    void attachmentMayAnswerAnotherMessage()
    {
        const MessageId target = MessageId::generate();
        const DeviceId author = DeviceId::generate();
        const auto message = MessageContent::attachmentMessage(
            QStringLiteral("This one?"), photo(), MessageQuote{target, author, QString(500, u'q')});
        QCOMPARE(message.quotedBody.size(), maxQuoteLength);
        const auto decoded = decodeMessageContent(encodeMessageContent(message));
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == message);
        QCOMPARE(decoded->type, MessageContent::Type::Attachment);
        QCOMPARE(*decoded->target, target);
        QCOMPARE(*decoded->quotedSender, author);
        QCOMPARE(decoded->quotedBody, QString(maxQuoteLength, u'q'));
        QCOMPARE(*decoded->attachment, *message.attachment);
    }

    void longestAttachmentMessageStillFits()
    {
        // Every field at its limit, in three-byte UTF-8 where text is.
        AttachmentDescriptor descriptor = photo();
        descriptor.kind = AttachmentKind::File;
        descriptor.width = descriptor.height = 0;
        descriptor.hasPreview = false;
        descriptor.byteCount = AttachmentLimits::maxFileBytes;
        descriptor.partCount = AttachmentLimits::maxParts;
        descriptor.fileName
            = QString(AttachmentLimits::maxFileNameLength - 4, QChar(0x4E2D)) + QStringLiteral(".pdf");
        descriptor.mimeType
            = QStringLiteral("application/") + QString(AttachmentLimits::maxMimeLength - 12, u'x');
        const QString caption(65536, QChar(0x4E2D));
        const auto message = MessageContent::attachmentMessage(
            caption, descriptor,
            MessageQuote{MessageId::generate(), DeviceId::generate(), QString(200, QChar(0x4E2D))});
        const QByteArray bytes = encodeMessageContent(message);
        QVERIFY(!bytes.isEmpty());
        QVERIFY(bytes.size() <= AttachmentLimits::maxMessageBytes);
        const auto decoded = decodeMessageContent(bytes);
        QVERIFY(decoded.has_value());
        QVERIFY(*decoded == message);

        // One character more is refused before anything is sent, and by
        // every receiver.
        const auto tooLong = MessageContent::attachmentMessage(caption + u'x', descriptor);
        QVERIFY(encodeMessageContent(tooLong).isEmpty());
        QVERIFY(!decodeMessageContent(attachmentPayload(caption + u'x', descriptor.attachmentId.bytes(),
                                                        descriptorMap(descriptor))));
        // A payload over the cap is not even parsed.
        QVERIFY(!decodeMessageContent(attachmentPayload(QString(AttachmentLimits::maxMessageBytes, u'a'),
                                                        descriptor.attachmentId.bytes(),
                                                        descriptorMap(descriptor))));
    }

    void attachmentBoundsAreEnforced()
    {
        const AttachmentDescriptor good = photo();
        const QByteArray id = good.attachmentId.bytes();
        QVERIFY(decodeMessageContent(attachmentPayload(good, descriptorMap(good))));

        const auto refused = [&](const std::function<void(QCborMap &)> &change) {
            QCborMap map = descriptorMap(good);
            change(map);
            return !decodeMessageContent(attachmentPayload(good, map));
        };
        QVERIFY(refused([](QCborMap &map) { map.remove(1); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(1, 0); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(1, 16); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(1, QStringLiteral("image")); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(2, 0); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(2, -1); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(2, AttachmentLimits::maxImageBytes + 1); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(2, 1.5); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(4, 1); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(4, qint64(1) << 40); }));
        QVERIFY(refused([](QCborMap &map) { map.remove(3); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(3, QByteArray(31, 's')); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(5, QByteArray(31, 'k')); }));
        QVERIFY(refused([](QCborMap &map) { map.remove(5); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(8, AttachmentLimits::maxImageDimension + 1); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(9, -1); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(9, QStringLiteral("tall")); }));
        QVERIFY(refused([](QCborMap &map) { map.insert(8, qint64(1) << 33); }));

        // The envelope around the descriptor.
        const QCborMap map = descriptorMap(good);
        QVERIFY(!decodeMessageContent(attachmentPayload(QCborValue(7), id, map)));
        QVERIFY(!decodeMessageContent(attachmentPayload(QStringLiteral("c"), QByteArray(15, 'i'), map)));
        QVERIFY(!decodeMessageContent(attachmentPayload(QStringLiteral("c"), QByteArray(16, '\0'), map)));
        QVERIFY(!decodeMessageContent(
            attachmentPayload(QStringLiteral("c"), id, QCborValue(QCborArray{1, 2}))));
        QVERIFY(!decodeMessageContent(tagged({qint64(1), qint64(3), QStringLiteral("c"), id, map})));
        QVERIFY(!decodeMessageContent(
            tagged({qint64(2), qint64(3), QStringLiteral("c"), id, map, QCborValue(nullptr)})));
        // A reply that is not one.
        const QByteArray target = MessageId::generate().bytes();
        const QByteArray author = DeviceId::generate().bytes();
        QVERIFY(decodeMessageContent(attachmentPayload(QStringLiteral("c"), id, map,
                                                       QCborArray{target, author, QStringLiteral("q")})));
        QVERIFY(!decodeMessageContent(
            attachmentPayload(QStringLiteral("c"), id, map, QStringLiteral("reply"))));
        QVERIFY(!decodeMessageContent(
            attachmentPayload(QStringLiteral("c"), id, map, QCborArray{target, author})));
        QVERIFY(!decodeMessageContent(
            attachmentPayload(QStringLiteral("c"), id, map,
                              QCborArray{QByteArray(16, '\0'), author, QStringLiteral("q")})));
        QVERIFY(!decodeMessageContent(attachmentPayload(
            QStringLiteral("c"), id, map, QCborArray{target, QByteArray(3, 'd'), QStringLiteral("q")})));
        QVERIFY(!decodeMessageContent(attachmentPayload(
            QStringLiteral("c"), id, map, QCborArray{target, author, QString(maxQuoteLength + 1, u'q')})));
    }

    void decorationsAreDroppedNotFatal()
    {
        const AttachmentDescriptor good = song();
        const auto decodedWith = [&](const std::function<void(QCborMap &)> &change) {
            QCborMap map = descriptorMap(good);
            change(map);
            const auto decoded = decodeMessageContent(attachmentPayload(good, map));
            return decoded ? decoded->attachment : std::nullopt;
        };
        // A MIME type outside the grammar, a waveform too long or of the
        // wrong type, a preview flag that is not a boolean.
        auto decoded = decodedWith([](QCborMap &map) { map.insert(6, QStringLiteral("audio/<script>")); });
        QVERIFY(decoded);
        QVERIFY(decoded->mimeType.isEmpty());
        decoded = decodedWith([](QCborMap &map) { map.insert(6, 42); });
        QVERIFY(decoded);
        QVERIFY(decoded->mimeType.isEmpty());
        decoded = decodedWith(
            [](QCborMap &map) { map.insert(12, QByteArray(AttachmentLimits::maxPeaks + 1, 'p')); });
        QVERIFY(decoded);
        QVERIFY(decoded->peaks.isEmpty());
        decoded = decodedWith([](QCborMap &map) { map.insert(12, QStringLiteral("peaks")); });
        QVERIFY(decoded);
        QVERIFY(decoded->peaks.isEmpty());
        decoded = decodedWith([](QCborMap &map) { map.insert(11, 1); });
        QVERIFY(decoded);
        QVERIFY(!decoded->hasPreview);
        // The name is always cleaned again, whatever the sender did.
        decoded = decodedWith([](QCborMap &map) { map.insert(7, QStringLiteral("../../\u202Egpj.exe")); });
        QVERIFY(decoded);
        QCOMPARE(decoded->fileName, QStringLiteral("_.._gpj.exe"));
        decoded = decodedWith([](QCborMap &map) { map.insert(7, QByteArray("name")); });
        QVERIFY(decoded);
        QVERIFY(decoded->fileName.isEmpty());
        decoded = decodedWith([](QCborMap &map) { map.insert(7, QString(500, u'n')); });
        QVERIFY(decoded);
        QCOMPARE(decoded->fileName.size(), AttachmentLimits::maxFileNameLength);
        // Fields that do not belong to the kind are ignored.
        decoded = decodedWith([](QCborMap &map) {
            map.insert(8, 640);
            map.insert(9, 480);
        });
        QVERIFY(decoded);
        QCOMPARE(decoded->width, 0);
        QCOMPARE(decoded->height, 0);
    }

    void unknownFieldsAreIgnored()
    {
        const AttachmentDescriptor good = photo();
        QCborMap map = descriptorMap(good);
        map.insert(99, QStringLiteral("a later key"));
        map.insert(QStringLiteral("text key"), 1);
        QCborArray fields = {qint64(1), qint64(3), QStringLiteral("c"), good.attachmentId.bytes(), map,
                             QCborValue(nullptr), QStringLiteral("appended")};
        const auto decoded = decodeMessageContent(tagged(fields));
        QVERIFY(decoded.has_value());
        QCOMPARE(*decoded->attachment, good);
    }

    void newerKindsArriveAsFiles()
    {
        for (qint64 kind = 5; kind <= 15; ++kind) {
            AttachmentDescriptor sent = song();
            QCborMap map = descriptorMap(sent);
            map.insert(1, kind);
            map.insert(8, 320);
            map.insert(11, true);
            const auto decoded = decodeMessageContent(attachmentPayload(sent, map));
            QVERIFY2(decoded.has_value(), qPrintable(QString::number(kind)));
            const AttachmentDescriptor &file = *decoded->attachment;
            QCOMPARE(file.kind, AttachmentKind::File);
            QCOMPARE(file.byteCount, sent.byteCount);
            QCOMPARE(file.sha256, sent.sha256);
            QCOMPARE(file.key, sent.key);
            QCOMPARE(file.fileName, sent.fileName);
            QCOMPARE(file.width, 0);
            QCOMPARE(file.durationMs, qint64(0));
            QVERIFY(file.peaks.isEmpty());
            QVERIFY(!file.hasPreview);
            QVERIFY(isValidDescriptor(file));
        }
        // Still held to a file's cap.
        AttachmentDescriptor big = song();
        big.byteCount = AttachmentLimits::maxFileBytes + 1;
        big.partCount = attachmentPartCount(big.byteCount);
        QCborMap map = descriptorMap(big);
        map.insert(1, 9);
        QVERIFY(!decodeMessageContent(attachmentPayload(big, map)));
    }

    void senderRefusesWhatReceiversWouldRefuse()
    {
        AttachmentDescriptor invalid = photo();
        invalid.partCount += 1;
        QVERIFY(encodeMessageContent(MessageContent::attachmentMessage(QStringLiteral("c"), invalid))
                    .isEmpty());
        invalid = photo();
        invalid.fileName = QStringLiteral("a/b.jpg");
        QVERIFY(encodeMessageContent(MessageContent::attachmentMessage(QStringLiteral("c"), invalid))
                    .isEmpty());
        invalid = photo();
        invalid.key.clear();
        QVERIFY(encodeMessageContent(MessageContent::attachmentMessage(QStringLiteral("c"), invalid))
                    .isEmpty());

        MessageContent noDescriptor;
        noDescriptor.type = MessageContent::Type::Attachment;
        noDescriptor.body = QStringLiteral("c");
        QVERIFY(encodeMessageContent(noDescriptor).isEmpty());
        // Half a quote.
        MessageContent halfQuote = MessageContent::attachmentMessage(QStringLiteral("c"), photo());
        halfQuote.target = MessageId::generate();
        QVERIFY(encodeMessageContent(halfQuote).isEmpty());
    }

    void olderClientsConsumeAttachmentsSilently()
    {
        const MessageQuote quote{MessageId::generate(), DeviceId::generate(), QStringLiteral("q")};
        for (const auto &message :
             {MessageContent::attachmentMessage(QStringLiteral("Look!"), photo()),
              MessageContent::attachmentMessage(QString(), song()),
              MessageContent::attachmentMessage(QStringLiteral("Look!"), photo(), quote)}) {
            const QByteArray bytes = encodeMessageContent(message);
            QVERIFY(!bytes.isEmpty());
            QVERIFY(!legacyClientShows(bytes));
        }
        // And this version still shows what they send.
        QVERIFY(legacyClientShows(encodeMessageContent(MessageContent::text(QStringLiteral("hi")))));
        QVERIFY(legacyClientShows(
            encodeMessageContent(MessageContent::edit(MessageId::generate(), QStringLiteral("x")))));
    }

    void repliesKeepTheirOwnSizeCap()
    {
        // A reply between the reply cap and the attachment cap is still
        // refused, as every earlier version refuses it.
        const QByteArray id = MessageId::generate().bytes();
        const QByteArray device = DeviceId::generate().bytes();
        const QString body(3 * (65536 + 200) + 2000, u'a');
        const QByteArray bytes = tagged({qint64(1), qint64(1), body, id, device, QStringLiteral("q")});
        QVERIFY(bytes.size() > 3 * (65536 + 200) + 1024);
        QVERIFY(bytes.size() <= AttachmentLimits::maxMessageBytes);
        QVERIFY(!decodeMessageContent(bytes));
    }

    void idIsTheCiphertextDigestPrefix()
    {
        const QByteArray ciphertext("an MLS private message", 22);
        const MessageId id = messageIdForCiphertext(ciphertext);
        QCOMPARE(id.bytes(),
                 QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256).left(16));
        QCOMPARE(messageIdForCiphertext(ciphertext), id);
        QVERIFY(messageIdForCiphertext(QByteArray("another", 7)) != id);
    }
};

QTEST_APPLESS_MAIN(MessageContentTest)
#include "tst_messagecontent.moc"
