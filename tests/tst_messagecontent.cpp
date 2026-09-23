#include <QtTest>

#include "domain/MessageContent.h"

#include <QCborArray>
#include <QCborValue>
#include <QCryptographicHash>

using namespace OpenChat;

namespace {

// A tagged payload built by hand, for shapes the encoder never produces.
QByteArray tagged(const QCborArray &fields)
{
    QByteArray bytes(1, '\xFF');
    bytes.append(QCborValue(fields).toCbor());
    return bytes;
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
