#include <QtTest>

#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"

using OpenChat::AvatarArtwork;
using OpenChat::BubbleBackground;

class BubbleBackgroundTest final : public QObject
{
    Q_OBJECT

private slots:
    void roundedAvatarClipRejectsSquareCorners()
    {
        const QRectF bounds(0, 0, 44, 44);
        const QPainterPath clip = AvatarArtwork::makeClipPath(bounds, 5);

        QVERIFY(clip.contains(QPointF(22, 22)));
        QVERIFY(!clip.contains(QPointF(0.5, 0.5)));
        QVERIFY(!clip.contains(QPointF(43.5, 43.5)));
        QVERIFY(clip.contains(QPointF(5, 1)));
    }

    void pathStaysInsideBoundsAndIncludesTail()
    {
        const QRectF bounds(0, 0, 320, 94);
        const QPainterPath path = BubbleBackground::makePath(bounds, false, 6, 9, 13);

        const QRectF strokeSafeBounds = bounds.adjusted(0.5, 0.5, -0.5, -0.5);
        QVERIFY(strokeSafeBounds.adjusted(-0.01, -0.01, 0.01, 0.01).contains(path.boundingRect()));
        QVERIFY(path.contains(QPointF(12, 12)));
        QVERIFY(path.contains(QPointF(5, 80)));
        QVERIFY(!path.contains(QPointF(2, 10)));
    }

    void directionsMirrorTailPlacement()
    {
        const QRectF bounds(0, 0, 240, 70);
        const QPainterPath incoming = BubbleBackground::makePath(bounds, false, 6, 9, 13);
        const QPainterPath outgoing = BubbleBackground::makePath(bounds, true, 6, 9, 13);

        QVERIFY(incoming.contains(QPointF(5, 56)));
        QVERIFY(!incoming.contains(QPointF(5, 10)));
        QVERIFY(outgoing.contains(QPointF(234, 56)));
        QVERIFY(!outgoing.contains(QPointF(235, 10)));
        QVERIFY(incoming.contains(QPointF(235, 10)));
        QVERIFY(outgoing.contains(QPointF(5, 10)));
        QCOMPARE(incoming.boundingRect().size(), outgoing.boundingRect().size());
    }

    void tinyAndTallPathsRemainUsable()
    {
        const QList<QSizeF> sizes = {QSizeF(110, 42), QSizeF(360, 58), QSizeF(360, 180)};
        for (const QSizeF &size : sizes) {
            const QRectF bounds(QPointF(), size);
            const QPainterPath path = BubbleBackground::makePath(bounds, false, 6, 9, 13);
            QVERIFY(!path.isEmpty());
            QVERIFY(path.contains(bounds.center()));
            const QRectF strokeSafeBounds = bounds.adjusted(0.5, 0.5, -0.5, -0.5);
            QVERIFY(
                strokeSafeBounds.adjusted(-0.01, -0.01, 0.01, 0.01).contains(path.boundingRect()));
        }
    }
};

QTEST_GUILESS_MAIN(BubbleBackgroundTest)

#include "tst_bubblebackground.moc"
