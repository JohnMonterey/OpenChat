#include <QGuiApplication>
#include <QImage>
#include <QSet>
#include <QtTest>

#include "cosmetics/AvatarFrameItem.h"
#include "cosmetics/BeadArtItem.h"
#include "cosmetics/CosmeticCatalog.h"
#include "cosmetics/ProfileSceneItem.h"

namespace {

// The average colour of the visibly painted pixels, weighted by coverage.
QColor averageInk(const QImage &image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    double r = 0, g = 0, b = 0, weight = 0;
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            const double a = qAlpha(line[x]) / 255.0;
            if (a < 0.6)
                continue;
            r += qRed(line[x]) * a;
            g += qGreen(line[x]) * a;
            b += qBlue(line[x]) * a;
            weight += a;
        }
    }
    if (weight <= 0)
        return {};
    return QColor(int(r / weight), int(g / weight), int(b / weight));
}

int opaquePixels(const QImage &image)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            count += qAlpha(line[x]) > 128 ? 1 : 0;
    }
    return count;
}

} // namespace

class CosmeticsTest final : public QObject
{
    Q_OBJECT

private slots:
    void catalogueIdsAreUniqueAndCategorised()
    {
        QSet<QString> seen;
        for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::all()) {
            QVERIFY2(!seen.contains(info.id), qPrintable(info.id));
            seen.insert(info.id);
            QVERIFY(info.id.startsWith(info.category + QLatin1Char('.')));
            QVERIFY(!info.name.isEmpty());
            QVERIFY(!info.description.isEmpty());
        }
        for (const char *category : {"frame", "bead", "flair", "scene"})
            QVERIFY(OpenChat::CosmeticCatalog::inCategory(QLatin1String(category)).size() >= 5);
        QVERIFY(OpenChat::CosmeticCatalog::isKnown(QStringLiteral("frame.aero"), QStringLiteral("frame")));
        QVERIFY(!OpenChat::CosmeticCatalog::isKnown(QStringLiteral("frame.aero"), QStringLiteral("bead")));
        QVERIFY(!OpenChat::CosmeticCatalog::isKnown(QString(), QStringLiteral("frame")));
    }

    // Every frame draws something round the picture and leaves its middle
    // untouched, at every size the interface uses.
    void framesDecorateTheEdgeAndLeaveTheFaceClear()
    {
        for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::inCategory(QStringLiteral("frame"))) {
            for (const qreal size : {32.0, 44.0, 74.0}) {
                const QMarginsF in = OpenChat::AvatarFrame::insetsFor(info.id, size);
                QVERIFY(in.left() >= 4 && in.left() <= 10);
                const QImage image = OpenChat::AvatarFrame::render(info.id, size, 5, false, 0, 2.0);
                QVERIFY2(!image.isNull(), qPrintable(info.id));
                QCOMPARE(image.width(), qCeil((size + in.left() + in.right()) * 2.0));
                QVERIFY2(opaquePixels(image) > size * 8, qPrintable(info.id));
                // The central 60% of the picture is never painted over.
                const QRect face(qRound((in.left() + size * 0.2) * 2), qRound((in.top() + size * 0.2) * 2),
                                 qRound(size * 0.6 * 2), qRound(size * 0.6 * 2));
                QCOMPARE(opaquePixels(image.copy(face)), 0);
            }
        }
    }

    void animatedFramesCycleAndStillFramesDoNot()
    {
        for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::inCategory(QStringLiteral("frame"))) {
            const int phases = OpenChat::AvatarFrame::phaseCountFor(info.id);
            QCOMPARE(phases > 1, info.animated);
            if (phases > 1) {
                const QImage a = OpenChat::AvatarFrame::render(info.id, 44, 5, true, 0, 1.0);
                const QImage b = OpenChat::AvatarFrame::render(info.id, 44, 5, true, 1, 1.0);
                QVERIFY(a != b);
                QCOMPARE(OpenChat::AvatarFrame::render(info.id, 44, 5, true, phases, 1.0), a);
            }
        }
    }

    // Whatever a bead is made of, Available reads green, Away amber, Busy red
    // and Offline grey.
    void beadStylesKeepThePresenceColour()
    {
        for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::inCategory(QStringLiteral("bead"))) {
            for (const bool dark : {false, true}) {
                const QColor available = averageInk(OpenChat::BeadArt::render(info.id, 0, 11, dark, 2.0));
                const QColor away = averageInk(OpenChat::BeadArt::render(info.id, 1, 11, dark, 2.0));
                const QColor offline = averageInk(OpenChat::BeadArt::render(info.id, 2, 11, dark, 2.0));
                const QColor busy = averageInk(OpenChat::BeadArt::render(info.id, 3, 11, dark, 2.0));
                const QByteArray id = info.id.toUtf8();
                QVERIFY2(available.green() > available.red() + 30 && available.green() > available.blue() + 30, id);
                QVERIFY2(away.red() > away.blue() + 60 && away.green() > away.blue() + 40, id);
                QVERIFY2(busy.red() > busy.green() + 60 && busy.red() > busy.blue() + 60, id);
                QVERIFY2(offline.hslSaturationF() < 0.25, id);
            }
        }
    }

    void scenesFillTheHeaderAndFadeAtTheBottom()
    {
        for (const OpenChat::CosmeticInfo &info : OpenChat::CosmeticCatalog::inCategory(QStringLiteral("scene"))) {
            const QImage image = OpenChat::ProfileScene::render(info.id, QSizeF(267, 76), true,
                                                                QRectF(64, 9, 120, 48), 12, 1.0)
                                     .convertToFormat(QImage::Format_ARGB32);
            QCOMPARE(qAlpha(image.pixel(4, 4)), 255);
            QCOMPARE(qAlpha(image.pixel(200, 40)), 255);
            QVERIFY(qAlpha(image.pixel(100, 75)) < 40);
        }
        QVERIFY(opaquePixels(OpenChat::ProfileScene::render(QString(), QSizeF(50, 50), false, {}, 12, 1.0)) == 0);
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    CosmeticsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_cosmetics.moc"
