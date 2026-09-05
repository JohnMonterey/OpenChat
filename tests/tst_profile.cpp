#include "domain/ProfileUpdate.h"
#include "models/Contact.h"
#include "render/AvatarStore.h"
#include "render/ProfileImage.h"

#include <QBuffer>
#include <QCborArray>
#include <QCborValue>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest/QTest>

using namespace OpenChat;

namespace {

// A busy picture: noisy enough that JPEG cannot compress it for free, so the
// size bound actually has to do some work.
QImage noisyImage(int width, int height, uint seed = 7)
{
    QImage image(width, height, QImage::Format_RGB32);
    uint state = seed;
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            state = state * 1'103'515'245u + 12'345u;
            line[x] = qRgb((state >> 16) & 0xff, (state >> 8) & 0xff, state & 0xff);
        }
    }
    return image;
}

QByteArray smallJpeg()
{
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 80);
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

class ProfileTest final : public QObject
{
    Q_OBJECT

private slots:
    void updateRoundTripsEveryField()
    {
        ProfileUpdateMessage message;
        message.presence = static_cast<int>(Presence::Busy);
        message.statusText = QStringLiteral("  In a meeting until 3  ");
        message.avatarJpeg = smallJpeg();

        const auto decoded = decodeProfileUpdate(encodeProfileUpdate(message));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->presence, static_cast<int>(Presence::Busy));
        QCOMPARE(decoded->statusText, QStringLiteral("In a meeting until 3"));
        QCOMPARE(decoded->avatarJpeg, message.avatarJpeg);

        // No picture and no status is a valid, minimal profile.
        const auto empty = decodeProfileUpdate(encodeProfileUpdate(ProfileUpdateMessage{}));
        QVERIFY(empty.has_value());
        QCOMPARE(*empty, ProfileUpdateMessage{});
    }

    void encodingCapsTheStatusLine()
    {
        ProfileUpdateMessage message;
        message.statusText = QString(maxStatusTextLength + 40, QLatin1Char('x'));
        const auto decoded = decodeProfileUpdate(encodeProfileUpdate(message));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->statusText.size(), maxStatusTextLength);
    }

    void malformedUpdatesAreRejected_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not cbor") << QByteArray("hello");
        QTest::newRow("not an array") << QCborValue(7).toCbor();
        QTest::newRow("wrong arity")
            << QCborValue(QCborArray{1, 0, QStringLiteral("hi")}).toCbor();
        QTest::newRow("wrong version")
            << QCborValue(QCborArray{2, 0, QStringLiteral("hi"), QByteArray()}).toCbor();
        QTest::newRow("presence out of range")
            << QCborValue(QCborArray{1, presenceCount, QStringLiteral("hi"), QByteArray()})
                   .toCbor();
        QTest::newRow("negative presence")
            << QCborValue(QCborArray{1, -1, QStringLiteral("hi"), QByteArray()}).toCbor();
        QTest::newRow("status not a string")
            << QCborValue(QCborArray{1, 0, 5, QByteArray()}).toCbor();
        QTest::newRow("status too long")
            << QCborValue(QCborArray{1, 0, QString(maxStatusTextLength + 1, QLatin1Char('s')),
                                     QByteArray()})
                   .toCbor();
        QTest::newRow("picture not bytes")
            << QCborValue(QCborArray{1, 0, QStringLiteral("hi"), QStringLiteral("jpeg")})
                   .toCbor();
        QTest::newRow("picture not a jpeg")
            << QCborValue(QCborArray{1, 0, QStringLiteral("hi"), QByteArray("PNG.....")})
                   .toCbor();
        QTest::newRow("picture too big")
            << QCborValue(QCborArray{1, 0, QStringLiteral("hi"),
                                     QByteArray("\xFF\xD8", 2) + QByteArray(maxAvatarJpegBytes, 'j')})
                   .toCbor();
    }

    void malformedUpdatesAreRejected()
    {
        QFETCH(QByteArray, bytes);
        QVERIFY(!decodeProfileUpdate(bytes).has_value());
    }

    void largePhotoBecomesASmallSquareJpeg()
    {
        // A 4000 x 3000 landscape photo: cropped to its centre square, scaled
        // to the output side, and compressed under the byte bound.
        const auto processed = processProfileImage(noisyImage(4000, 3000));
        QVERIFY2(processed.hasValue(), "a large photo must be accepted");
        const QByteArray jpeg = processed.value();
        QVERIFY(jpeg.size() <= ProfileImageLimits{}.maxOutputBytes);
        QVERIFY(jpeg.size() <= maxAvatarJpegBytes);
        QVERIFY(jpeg.startsWith(QByteArray("\xFF\xD8", 2)));

        const QImage decoded = QImage::fromData(jpeg, "JPEG");
        QVERIFY(!decoded.isNull());
        QCOMPARE(decoded.width(), decoded.height());
        QVERIFY(decoded.width() <= ProfileImageLimits{}.outputSide);
    }

    void simplePictureKeepsFullSize()
    {
        QImage image(600, 600, QImage::Format_RGB32);
        image.fill(QColor("#78acd3"));
        const auto processed = processProfileImage(image);
        QVERIFY(processed.hasValue());
        const QImage decoded = QImage::fromData(processed.value(), "JPEG");
        QCOMPARE(decoded.width(), ProfileImageLimits{}.outputSide);
        QCOMPARE(decoded.height(), ProfileImageLimits{}.outputSide);
    }

    void transparentPictureIsCompositedOnWhite()
    {
        QImage image(100, 100, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        const auto processed = processProfileImage(image);
        QVERIFY(processed.hasValue());
        const QImage decoded = QImage::fromData(processed.value(), "JPEG");
        const QColor centre = decoded.pixelColor(decoded.width() / 2, decoded.height() / 2);
        QVERIFY(centre.red() > 240 && centre.green() > 240 && centre.blue() > 240);
    }

    void unrealisticImagesAreRefused()
    {
        auto tooSmall = processProfileImage(noisyImage(8, 8));
        QVERIFY(!tooSmall.hasValue());
        QCOMPARE(tooSmall.error(), ProfileImageError::TooSmall);

        ProfileImageLimits tight;
        tight.maxSide = 500;
        auto tooLarge = processProfileImage(noisyImage(600, 100), tight);
        QVERIFY(!tooLarge.hasValue());
        QCOMPARE(tooLarge.error(), ProfileImageError::TooLarge);

        auto nothing = processProfileImage(QImage());
        QVERIFY(!nothing.hasValue());
        QCOMPARE(nothing.error(), ProfileImageError::Unreadable);
    }

    void filesAreCheckedBeforeDecoding()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        auto missing = processProfileImageFile(dir.filePath(QStringLiteral("absent.png")));
        QVERIFY(!missing.hasValue());
        QCOMPARE(missing.error(), ProfileImageError::FileMissing);

        const QString text = dir.filePath(QStringLiteral("notes.txt"));
        QVERIFY(writeFile(text, QByteArray("this is not a picture")));
        auto unreadable = processProfileImageFile(text);
        QVERIFY(!unreadable.hasValue());
        QCOMPARE(unreadable.error(), ProfileImageError::Unreadable);

        // The on-disk size is checked before the file is even opened as an
        // image: a perfectly valid picture over the bound is refused.
        const QString photo = dir.filePath(QStringLiteral("photo.png"));
        QVERIFY(noisyImage(300, 200).save(photo, "PNG"));
        ProfileImageLimits tiny;
        tiny.maxFileBytes = 1024;
        auto tooBig = processProfileImageFile(photo, tiny);
        QVERIFY(!tooBig.hasValue());
        QCOMPARE(tooBig.error(), ProfileImageError::FileTooLarge);

        // The header's dimensions are checked before the pixels are decoded.
        ProfileImageLimits narrow;
        narrow.maxSide = 250;
        auto tooWide = processProfileImageFile(photo, narrow);
        QVERIFY(!tooWide.hasValue());
        QCOMPARE(tooWide.error(), ProfileImageError::TooLarge);

        // And a real photo file goes through the whole pipeline.
        auto processed = processProfileImageFile(photo);
        QVERIFY(processed.hasValue());
        QVERIFY(processed.value().size() <= ProfileImageLimits{}.maxOutputBytes);
        const QImage decoded = QImage::fromData(processed.value(), "JPEG");
        QCOMPARE(decoded.width(), 200);
        QCOMPARE(decoded.height(), 200);

        // A large JPEG is downscaled on decode and still lands on the output side.
        const QString big = dir.filePath(QStringLiteral("big.jpg"));
        QVERIFY(noisyImage(3000, 2400).save(big, "JPEG", 90));
        auto bigProcessed = processProfileImageFile(big);
        QVERIFY(bigProcessed.hasValue());
        const QImage bigDecoded = QImage::fromData(bigProcessed.value(), "JPEG");
        QCOMPARE(bigDecoded.width(), bigDecoded.height());
        QVERIFY(bigDecoded.width() <= ProfileImageLimits{}.outputSide);
        QVERIFY(bigDecoded.width() >= ProfileImageLimits{}.outputSide / 4);
    }

    void storeIsContentAddressed()
    {
        AvatarStore &store = AvatarStore::instance();
        store.clear();
        const QByteArray jpeg = smallJpeg();

        QVERIFY(!store.image(QStringLiteral("userpfp_none")).has_value());
        QVERIFY(!AvatarStore::isBlobKey(QStringLiteral("michael")));
        QVERIFY(store.registerJpeg(QByteArray()).isEmpty());
        QVERIFY(store.registerJpeg(QByteArray("not a jpeg")).isEmpty());

        const QString key = store.registerJpeg(jpeg);
        QVERIFY(AvatarStore::isBlobKey(key));
        QCOMPARE(key, AvatarStore::keyFor(jpeg));
        QCOMPARE(store.registerJpeg(jpeg), key); // idempotent
        const auto image = store.image(key);
        QVERIFY(image.has_value());
        QCOMPARE(image->width(), 32);

        // Different bytes, different key: a changed picture is a changed key.
        QImage other(32, 32, QImage::Format_RGB32);
        other.fill(Qt::red);
        QByteArray otherBytes;
        QBuffer buffer(&otherBytes);
        buffer.open(QIODevice::WriteOnly);
        other.save(&buffer, "JPEG", 80);
        const QString otherKey = store.registerJpeg(otherBytes);
        QVERIFY(!otherKey.isEmpty());
        QVERIFY(otherKey != key);

        // Anything over the wire bound is refused without decoding.
        QVERIFY(store.registerJpeg(jpeg + QByteArray(maxAvatarJpegBytes, ' ')).isEmpty());
        store.clear();
        QVERIFY(!store.image(key).has_value());
    }
};

QTEST_GUILESS_MAIN(ProfileTest)
#include "tst_profile.moc"
