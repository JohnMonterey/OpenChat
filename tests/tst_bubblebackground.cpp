#include <QtTest>

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <vector>

#include "cosmetics/BubbleSkins.h"
#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"

using OpenChat::AvatarArtwork;
using OpenChat::BubbleBackground;
namespace BubbleSkins = OpenChat::BubbleSkins;

namespace {

// Lays a bubble out the way BubbleBackground does for a skin.
OpenChat::BubbleShape shapeFor(const QSizeF &size, bool outgoing)
{
    const QRectF bounds(QPointF(), size);
    const QRectF safe = bounds.adjusted(0.5, 0.5, -0.5, -0.5);
    OpenChat::BubbleShape shape;
    shape.path = BubbleBackground::makePath(bounds, outgoing, 6, 9, 13);
    shape.bounds = bounds;
    shape.body = outgoing ? safe.adjusted(0, 0, -9, 0) : safe.adjusted(9, 0, 0, 0);
    shape.outgoing = outgoing;
    shape.radius = 6;
    return shape;
}

QImage renderSkin(const QString &id, const QSizeF &size, bool outgoing, qreal ratio)
{
    QImage image((size * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    BubbleSkins::paint(&painter, id, shapeFor(size, outgoing), ratio);
    return image;
}

double linear(double channel)
{
    return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor &color)
{
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF())
           + 0.0722 * linear(color.blueF());
}

double contrast(double a, double b)
{
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

} // namespace

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

    void skinCatalogueListsDistinctSkinsAndLeavesClassicAlone()
    {
        const QList<OpenChat::BubbleSkinInfo> skins = BubbleSkins::catalog();
        QVERIFY(skins.size() >= 5);
        QSet<QString> ids;
        for (const OpenChat::BubbleSkinInfo &skin : skins) {
            QVERIFY2(skin.id.startsWith(QStringLiteral("bubble.")), qPrintable(skin.id));
            QVERIFY(!skin.name.isEmpty());
            QVERIFY(!skin.description.isEmpty());
            QVERIFY(BubbleSkins::isSkin(skin.id));
            QVERIFY(BubbleSkins::textColor(skin.id).isValid());
            QVERIFY(BubbleSkins::secondaryTextColor(skin.id).isValid());
            ids.insert(skin.id);
        }
        QCOMPARE(ids.size(), skins.size());

        for (const QString &classic : {QString(), QStringLiteral("classic"),
                                       QStringLiteral("bubble.unknown")}) {
            QVERIFY(!BubbleSkins::isSkin(classic));
            QVERIFY(!BubbleSkins::textColor(classic).isValid());
            QVERIFY(BubbleSkins::texture(classic).isNull());
            // Painting an unknown skin leaves the device untouched.
            const QImage untouched = renderSkin(classic, QSizeF(120, 54), true, 1.0);
            QCOMPARE(untouched.pixel(60, 27), qRgba(0, 0, 0, 0));
        }
    }

    void skinsFillTheOutlineAndNothingElse()
    {
        for (const OpenChat::BubbleSkinInfo &skin : BubbleSkins::catalog()) {
            for (const qreal ratio : {1.0, 2.0}) {
                for (const bool outgoing : {false, true}) {
                    const QSizeF size(240, 70);
                    const QImage image = renderSkin(skin.id, size, outgoing, ratio);
                    const auto alphaAt = [&](qreal x, qreal y) {
                        return qAlpha(image.pixel(qFloor(x * ratio), qFloor(y * ratio)));
                    };
                    QCOMPARE(alphaAt(120, 35), 255);
                    QCOMPARE(alphaAt(outgoing ? 20 : 220, 8), 255);
                    // The tail column above the tail stays empty, as do the corners.
                    QCOMPARE(alphaAt(outgoing ? 237 : 2, 10), 0);
                    QCOMPARE(alphaAt(outgoing ? 0.6 : 239.4, 0.6), 0);
                    // The tail itself is filled.
                    QCOMPARE(alphaAt(outgoing ? 234 : 5, 56), 255);
                }
            }
        }
    }

    void skinTexturesTileWithoutASeam()
    {
        for (const OpenChat::BubbleSkinInfo &skin : BubbleSkins::catalog()) {
            const QImage tile = BubbleSkins::texture(skin.id);
            QVERIFY(!tile.isNull());
            QCOMPARE(tile.width(), tile.height());
            // The jump across the wrap should look like any step between
            // neighbouring pixels inside the tile, not like an edge.
            const auto columnStep = [&tile](int a, int b) {
                double total = 0;
                for (int y = 0; y < tile.height(); ++y) {
                    const QRgb p = tile.pixel(a, y);
                    const QRgb q = tile.pixel(b, y);
                    total += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                             + std::abs(qBlue(p) - qBlue(q));
                }
                return total / tile.height();
            };
            const auto rowStep = [&tile](int a, int b) {
                double total = 0;
                for (int x = 0; x < tile.width(); ++x) {
                    const QRgb p = tile.pixel(x, a);
                    const QRgb q = tile.pixel(x, b);
                    total += std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q))
                             + std::abs(qBlue(p) - qBlue(q));
                }
                return total / tile.width();
            };
            const int last = tile.width() - 1;
            double largestColumnStep = 0;
            double largestRowStep = 0;
            // Odd starting rows and columns, so each pair straddles a boundary
            // of the 2 px glitter grid just as the wrap does.
            for (int i = 1; i < 64; ++i) {
                const int at = (last * i / 64) | 1;
                largestColumnStep = std::max(largestColumnStep, columnStep(at, at + 1));
                largestRowStep = std::max(largestRowStep, rowStep(at, at + 1));
            }
            QVERIFY2(columnStep(last, 0) <= largestColumnStep * 1.25 + 1.0, qPrintable(skin.id));
            QVERIFY2(rowStep(last, 0) <= largestRowStep * 1.25 + 1.0, qPrintable(skin.id));
        }
    }

    void skinnedTextStaysReadable()
    {
        // Measured over the area the message text actually occupies: the body
        // inside the delegate's 14 px padding, at a short and a wrapped size.
        for (const OpenChat::BubbleSkinInfo &skin : BubbleSkins::catalog()) {
            const double text = relativeLuminance(BubbleSkins::textColor(skin.id));
            const double secondary = relativeLuminance(BubbleSkins::secondaryTextColor(skin.id));
            for (const QSizeF size : {QSizeF(180, 54), QSizeF(300, 96)}) {
                const QImage image = renderSkin(skin.id, size, true, 2.0);
                std::vector<double> luminances;
                double sum = 0;
                for (int y = 8 * 2; y < (size.height() - 8) * 2; ++y) {
                    for (int x = 14 * 2; x < (size.width() - 9 - 14) * 2; ++x) {
                        const double l = relativeLuminance(QColor::fromRgb(image.pixel(x, y)));
                        luminances.push_back(l);
                        sum += l;
                    }
                }
                const double mean = sum / luminances.size();
                std::sort(luminances.begin(), luminances.end());
                // The background pixels least favourable to this text colour.
                const bool lightText = text > mean;
                const double worst = lightText
                    ? luminances[luminances.size() * 95 / 100]
                    : luminances[luminances.size() * 5 / 100];
                const double textContrast = contrast(text, mean);
                const double secondaryContrast = contrast(secondary, mean);
                const double worstContrast = contrast(text, worst);
                qInfo("%s %gx%g: text %.2f (5%% worst %.2f), secondary %.2f",
                      qPrintable(skin.id), size.width(), size.height(), textContrast,
                      worstContrast, secondaryContrast);
                QVERIFY2(textContrast >= 7.0, qPrintable(skin.id));
                QVERIFY2(worstContrast >= 3.0, qPrintable(skin.id));
                QVERIFY2(secondaryContrast >= 3.0, qPrintable(skin.id));
            }
        }
    }
};

QTEST_GUILESS_MAIN(BubbleBackgroundTest)

#include "tst_bubblebackground.moc"
