#include <QBuffer>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"
#include "profile/ProfileAmbientItem.h"
#include "profile/ProfileBackdropItem.h"
#include "profile/ProfileBackgroundImage.h"
#include "profile/ProfileFonts.h"
#include "profile/ProfileImageLayerItem.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfileMoodFaceItem.h"
#include "profile/ProfileMotifs.h"
#include "profile/ProfileNameTextItem.h"
#include "profile/ProfilePresetThumbItem.h"
#include "profile/ProfileQmlTypes.h"
#include "profile/ProfileReadability.h"
#include "profile/ProfileRenderPolicy.h"
#include "profile/ProfileTicker.h"

using namespace OpenChat;
namespace Readability = OpenChat::ProfileReadability;
namespace Motifs = OpenChat::ProfileMotifs;
using Profile::Motif;

namespace {

QColor rgb(quint32 value)
{
    return Readability::rgb(value);
}

bool near(const QColor &a, const QColor &b, int tolerance)
{
    return std::abs(a.red() - b.red()) <= tolerance && std::abs(a.green() - b.green()) <= tolerance
           && std::abs(a.blue() - b.blue()) <= tolerance;
}

QColor at(const QImage &image, int x, int y)
{
    return QColor::fromRgba(image.pixel(x, y));
}

QString hex(const QColor &c)
{
    return c.name(QColor::HexArgb);
}

// The share of pixels that differ from `base` by more than `threshold` in
// some channel.
double inkedShare(const QImage &image, const QColor &base, int threshold = 40)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    qint64 inked = 0;
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (std::abs(qRed(line[x]) - base.red()) > threshold || std::abs(qGreen(line[x]) - base.green()) > threshold
                || std::abs(qBlue(line[x]) - base.blue()) > threshold)
                ++inked;
        }
    }
    return double(inked) / (double(argb.width()) * argb.height());
}

int pixelsNear(const QImage &image, const QColor &colour, int tolerance)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            count += near(QColor::fromRgb(line[x]), colour, tolerance) ? 1 : 0;
    }
    return count;
}

int opaquePixels(const QImage &image, int minimumAlpha = 32)
{
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x)
            count += qAlpha(line[x]) >= minimumAlpha ? 1 : 0;
    }
    return count;
}

int differingPixels(const QImage &a, const QImage &b, int tolerance = 8)
{
    const QImage x = a.convertToFormat(QImage::Format_ARGB32);
    const QImage y = b.convertToFormat(QImage::Format_ARGB32);
    if (x.size() != y.size())
        return std::max(x.width() * x.height(), y.width() * y.height());
    int count = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto *l = reinterpret_cast<const QRgb *>(x.constScanLine(row));
        const auto *r = reinterpret_cast<const QRgb *>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            if (std::abs(qRed(l[col]) - qRed(r[col])) > tolerance || std::abs(qGreen(l[col]) - qGreen(r[col])) > tolerance
                || std::abs(qBlue(l[col]) - qBlue(r[col])) > tolerance
                || std::abs(qAlpha(l[col]) - qAlpha(r[col])) > tolerance)
                ++count;
        }
    }
    return count;
}

QImage renderBackdrop(const Motifs::BackdropSpec &spec, const QSize &size, qreal zoom = 1.0)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    Motifs::paintBackdrop(painter, QRectF(QPointF(0, 0), QSizeF(size)), spec, zoom);
    return image;
}

QImage paintItem(QQuickPaintedItem &item)
{
    QImage image(item.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    item.paint(&painter);
    return image;
}

QByteArray jpegOf(const QImage &image, int quality = 92)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", quality);
    return bytes;
}

QImage decodeJpeg(const QByteArray &jpeg)
{
    return QImage::fromData(jpeg, "JPEG");
}

// A real baseline JPEG with its scan repeated until it has `scans` of them:
// what a hostile "scan bomb" looks like to the marker walk.
QByteArray withRepeatedScan(const QByteArray &jpeg, int scans)
{
    const qsizetype start = jpeg.indexOf(QByteArray("\xFF\xDA", 2));
    const qsizetype end = jpeg.lastIndexOf(QByteArray("\xFF\xD9", 2));
    const QByteArray scan = jpeg.mid(start, end - start);
    QByteArray out = jpeg.left(end);
    for (int i = 1; i < scans; ++i)
        out += scan;
    out += QByteArray("\xFF\xD9", 2);
    return out;
}

bool hasMarker(const QByteArray &jpeg, quint8 marker)
{
    // Markers before the first scan (the entropy data after it is skipped).
    qsizetype i = 2;
    while (i + 4 <= jpeg.size() && quint8(jpeg[i]) == 0xFF) {
        const quint8 code = quint8(jpeg[i + 1]);
        if (code == marker)
            return true;
        if (code == 0xDA)
            return false;
        const int length = (quint8(jpeg[i + 2]) << 8) | quint8(jpeg[i + 3]);
        i += 2 + length;
    }
    return false;
}

// Smooth colour with grain on top: compresses like a photo.
QImage photoLike(int width, int height, quint32 seed, int grain)
{
    QImage image(width, height, QImage::Format_RGB32);
    QRandomGenerator random(seed);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int n = grain > 0 ? int(random.bounded(2 * grain + 1)) - grain : 0;
            line[x] = qRgb(std::clamp(40 + x * 180 / width + n, 0, 255), std::clamp(90 + y * 120 / height + n, 0, 255),
                           std::clamp(200 - x * 120 / width + n, 0, 255));
        }
    }
    return image;
}

QImage halves(int width, int height, const QColor &left, const QColor &right)
{
    QImage image(width, height, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(0, 0, width / 2, height, left);
    painter.fillRect(width / 2, 0, width - width / 2, height, right);
    return image;
}

double minContrast(const QColor &ink, const QVector<QColor> &backgrounds)
{
    double worst = 99;
    for (const QColor &bg : backgrounds)
        worst = std::min(worst, Readability::contrastRatio(ink, bg));
    return worst;
}

QVector<QColor> stops(const Readability::StripRecipe &recipe)
{
    return {recipe.stops[0], recipe.stops[1], recipe.stops[2], recipe.stops[3]};
}

Motifs::BackdropSpec specOf(const Profile::Theme &theme)
{
    Motifs::BackdropSpec spec;
    spec.kind = theme.backgroundKind;
    spec.color1 = rgb(theme.backgroundColor1);
    spec.color2 = rgb(theme.backgroundColor2);
    spec.motif = theme.motif;
    spec.scale = theme.motifScale;
    spec.ink = rgb(theme.motifInk);
    spec.opacity = theme.motifOpacity / 100.0;
    return spec;
}

// Review PNGs, only when OPENCHAT_PROFILE_CAPTURES names a directory.
void capture(const QString &name, const QImage &image)
{
    const QString directory = qEnvironmentVariable("OPENCHAT_PROFILE_CAPTURES");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    image.save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

QString motifSlug(Motif motif)
{
    static const QRegularExpression separators(QStringLiteral("[^a-z0-9]+"));
    return Profile::motifName(motif).toLower().replace(separators, QStringLiteral("-"));
}

QString presetSlugOf(int preset)
{
    return Profile::presetSlug(Profile::Preset(preset));
}

// A window on the offscreen platform and the software scene graph.
struct Stage final {
    QQuickWindow window;

    explicit Stage(const QSize &size)
    {
        window.resize(size);
        window.setColor(Qt::green);
    }

    template<typename T>
    T *add(const QSizeF &size)
    {
        auto *item = new T(window.contentItem());
        if (!size.isEmpty())
            item->setSize(size); // otherwise the item keeps its implicit size
        return item;
    }

    bool show()
    {
        window.show();
        return QTest::qWaitForWindowExposed(&window);
    }
};

} // namespace

class ProfileRenderTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        registerProfileQmlTypes();
    }

    void init()
    {
        ProfileRenderPolicy::instance().resetForTesting();
        ProfileRenderPolicy::instance().setReducedMotion(false);
        ProfileMediaStore::instance().resetForTesting();
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
    }

    void cleanup()
    {
        QThreadPool::globalInstance()->waitForDone(30000);
        QCoreApplication::processEvents();
    }

    // Registration and start-up never load the fonts; the first page does.
    void bundledFontsRegisterLazily()
    {
        const QStringList bundled = ProfileFonts::bundledFamilies();
        QCOMPARE(bundled.size(), 11);
        if (QFontDatabase::families().contains(QStringLiteral("Pacifico")))
            QSKIP("Pacifico is installed on this system, so its absence cannot be observed");
        // registerProfileQmlTypes() ran in initTestCase().
        QVERIFY(!ProfileFonts::isRegistered());
        for (const QString &family : bundled)
            QVERIFY2(!QFontDatabase::families().contains(family), qPrintable(family));

        ProfileFonts::ensureRegistered();
        QVERIFY(ProfileFonts::isRegistered());
        const QStringList families = QFontDatabase::families();
        for (const QString &family : bundled)
            QVERIFY2(families.contains(family), qPrintable(family));
        ProfileFonts::ensureRegistered(); // idempotent

        // Every face and role names a registered family, and the reverse
        // lookup finds it again.
        for (int f = 1; f <= int(Profile::Font::MarkerFont); ++f) {
            const auto font = Profile::Font(f);
            for (const auto role : {ProfileFonts::Role::Body, ProfileFonts::Role::Heading, ProfileFonts::Role::Name}) {
                const QString family = ProfileFonts::family(font, role);
                QVERIFY2(families.contains(family), qPrintable(family));
                QCOMPARE(ProfileFonts::fontForFamily(family), std::optional<Profile::Font>(font));
            }
        }
        QCOMPARE(ProfileFonts::family(Profile::Font::InterfaceFont, ProfileFonts::Role::Name), QString());
        QCOMPARE(ProfileFonts::family(Profile::Font::RoundedFont, ProfileFonts::Role::Body),
                 QStringLiteral("Fredoka Medium"));
        QCOMPARE(ProfileFonts::family(Profile::Font::RoundedFont, ProfileFonts::Role::Label),
                 QStringLiteral("Fredoka SemiBold"));
        QCOMPARE(ProfileFonts::family(Profile::Font::FutureFont, ProfileFonts::Role::Heading),
                 QStringLiteral("Orbitron SemiBold"));
        QCOMPARE(ProfileFonts::family(Profile::Font::FutureFont, ProfileFonts::Role::Name),
                 QStringLiteral("Orbitron ExtraBold"));
        QVERIFY(ProfileFonts::useBold(Profile::Font::TypewriterFont, ProfileFonts::Role::Label));
        QVERIFY(!ProfileFonts::useBold(Profile::Font::TypewriterFont, ProfileFonts::Role::Body));
        QVERIFY(!ProfileFonts::useBold(Profile::Font::SerifFont, ProfileFonts::Role::Heading));
        QCOMPARE(ProfileFonts::sizeFactor(Profile::Font::RoundedFont, ProfileFonts::Role::Body), 1.08);
        QCOMPARE(ProfileFonts::sizeFactor(Profile::Font::GothicFont, ProfileFonts::Role::Name), 1.18);
        QCOMPARE(ProfileFonts::sizeFactor(Profile::Font::FutureFont, ProfileFonts::Role::Heading), 0.86);
    }

    // ------------------------------------------------------------ motifs

    void everyMotifDrawsInkAtEveryScale_data()
    {
        QTest::addColumn<int>("motif");
        QTest::addColumn<int>("scale");
        for (int m = 0; m <= int(Motif::LinenWeave); ++m) {
            for (int s = 0; s <= int(Profile::MotifScale::LargeMotif); ++s) {
                const QByteArray name = motifSlug(Motif(m)).toUtf8() + "/" + QByteArray(1, "SML"[s]);
                QTest::newRow(name.constData()) << m << s;
            }
        }
    }

    void everyMotifDrawsInkAtEveryScale()
    {
        QFETCH(int, motif);
        QFETCH(int, scale);
        Motifs::BackdropSpec spec;
        spec.kind = Profile::BackgroundKind::PatternBackground;
        spec.color1 = spec.color2 = QColor(255, 255, 255);
        spec.motif = Motif(motif);
        spec.scale = Profile::MotifScale(scale);
        spec.ink = QColor(0, 0, 0);
        spec.opacity = 0.8;
        const QImage image = renderBackdrop(spec, QSize(480, 360));
        const double share = inkedShare(image, spec.color1);
        QVERIFY2(share > 0.01, qPrintable(QStringLiteral("only %1 of the pixels are inked").arg(share)));
        QVERIFY2(share < 0.95, qPrintable(QStringLiteral("%1 of the pixels are inked").arg(share)));
        // Solid-ink motifs draw their main shapes in the ink premixed with the
        // base at the pattern opacity: #333333 over white at 80%.
        static const QList<Motif> solidInk{Motif::Stars,      Motif::Hearts,     Motif::Skulls,  Motif::Checkerboard,
                                           Motif::Zebra,      Motif::Leopard,    Motif::PolkaDots, Motif::Sparkles,
                                           Motif::MusicNotes, Motif::Flowers,    Motif::Halftone,  Motif::BrokenHearts};
        if (solidInk.contains(Motif(motif)))
            QVERIFY(pixelsNear(image, QColor(0x33, 0x33, 0x33), 2) > 40);
        // The scale is real: the same motif at another scale draws differently.
        spec.scale = Profile::MotifScale((scale + 1) % 3);
        QVERIFY(differingPixels(image, renderBackdrop(spec, QSize(480, 360))) > 200);
    }

    // The viewport motifs repaint a whole window on every resize, so they
    // must stay cheap (a path per halftone dot took seconds on a 4K screen).
    void viewportMotifsPaintQuickly()
    {
        for (const Motif motif : {Motif::Halftone, Motif::Zebra, Motif::Bubbles, Motif::CyberGrid}) {
            Motifs::BackdropSpec spec;
            spec.motif = motif;
            spec.scale = Profile::MotifScale::SmallMotif;
            QElapsedTimer timer;
            timer.start();
            const QImage image = renderBackdrop(spec, QSize(1920, 1080));
            QVERIFY2(timer.elapsed() < 400, qPrintable(QStringLiteral("%1: %2 ms").arg(motifSlug(motif)).arg(timer.elapsed())));
            QVERIFY(inkedShare(image, spec.color1, 6) > 0.01);
        }
    }

    void motifTilesAreTransparentWithCutHoles()
    {
        const QColor ink(0xC0, 0x10, 0x60);
        const int opaque = qRound(0.6 * 255);
        for (int m = 0; m <= int(Motif::LinenWeave); ++m) {
            const auto motif = Motif(m);
            const QImage tile = Motifs::motifTile(motif, ink, 0.6, Profile::MotifScale::MediumMotif, 1.0);
            if (!Motifs::isTileMotif(motif)) {
                QVERIFY(tile.isNull());
                continue;
            }
            QVERIFY2(!tile.isNull(), qPrintable(motifSlug(motif)));
            const QImage argb = tile.convertToFormat(QImage::Format_ARGB32);
            int clear = 0;
            int inked = 0;
            for (int y = 0; y < argb.height(); ++y) {
                for (int x = 0; x < argb.width(); ++x) {
                    const int alpha = qAlpha(argb.pixel(x, y));
                    clear += alpha == 0 ? 1 : 0;
                    inked += alpha > 0 ? 1 : 0;
                }
            }
            // Transparent between the shapes: the base shows through.
            QVERIFY2(clear > 0 && inked > 0, qPrintable(motifSlug(motif)));
        }

        // Skull eyes are holes in an inked skull.
        const QImage skulls = Motifs::motifTile(Motif::Skulls, ink, 0.6, Profile::MotifScale::MediumMotif, 1.0)
                                  .convertToFormat(QImage::Format_ARGB32);
        const qreal T = 168;
        const qreal s = 0.3 * T;
        QTransform skull;
        skull.translate(0.26 * T, 0.3 * T);
        skull.rotateRadians(-0.18);
        const QPoint eye = skull.map(QPointF(-0.15 * s, -0.04 * s)).toPoint();
        const QPoint dome = skull.map(QPointF(0, -0.3 * s)).toPoint();
        QCOMPARE(qAlpha(skulls.pixel(eye)), 0);
        QVERIFY(std::abs(qAlpha(skulls.pixel(dome)) - opaque) <= 1);
        QVERIFY(near(QColor::fromRgb(skulls.pixel(dome)), ink, 2));

        // A heart's crack is cut right through it (and the stripes under it).
        const QImage hearts = Motifs::motifTile(Motif::BrokenHearts, ink, 0.6, Profile::MotifScale::MediumMotif, 1.0)
                                  .convertToFormat(QImage::Format_ARGB32);
        const qreal H = 150;
        const qreal size = 0.3 * H;
        QTransform heart;
        heart.translate(0.26 * H, 0.3 * H);
        heart.rotateRadians(-0.2);
        QCOMPARE(qAlpha(hearts.pixel(heart.map(QPointF(-0.04 * size, -0.15 * size)).toPoint())), 0);
        QVERIFY(std::abs(qAlpha(hearts.pixel(heart.map(QPointF(0.18 * size, 0.05 * size)).toPoint())) - opaque) <= 1);

        // Over any base, gradients included, the tile equals the ink premixed
        // with that base at the opacity.
        Motifs::BackdropSpec spec;
        spec.kind = Profile::BackgroundKind::PatternBackground;
        spec.color1 = QColor(0x10, 0x20, 0x80);
        spec.color2 = QColor(0xF0, 0xE0, 0x40);
        spec.motif = Motif::Stars;
        spec.ink = ink;
        spec.opacity = 0.6;
        const QImage patterned = renderBackdrop(spec, QSize(260, 520)).convertToFormat(QImage::Format_ARGB32);
        spec.kind = Profile::BackgroundKind::GradientBackground;
        const QImage base = renderBackdrop(spec, QSize(260, 520)).convertToFormat(QImage::Format_ARGB32);
        const QImage stars = Motifs::motifTile(Motif::Stars, ink, 0.6, Profile::MotifScale::MediumMotif, 1.0)
                                 .convertToFormat(QImage::Format_ARGB32);
        int checked = 0;
        for (int y = 0; y < patterned.height(); y += 3) {
            for (int x = 0; x < patterned.width(); x += 3) {
                if (qAlpha(stars.pixel(x % stars.width(), y % stars.height())) != opaque)
                    continue;
                const QColor expected = Readability::mix(QColor::fromRgb(base.pixel(x, y)), ink, 0.6);
                QVERIFY2(near(QColor::fromRgb(patterned.pixel(x, y)), expected, 2),
                         qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
                ++checked;
            }
        }
        QVERIFY(checked > 50);
        QVERIFY(Motifs::cachedTileCount() > 0);

        if (!qEnvironmentVariableIsEmpty("OPENCHAT_PROFILE_CAPTURES"))
            captureMotifs();
    }

    void backdropKindsPaintExpectedPixels()
    {
        ProfileBackdrop backdrop;
        backdrop.setSize(QSizeF(300, 400));

        // Solid: one colour, whatever the pattern fields say.
        backdrop.setKind(int(Profile::BackgroundKind::SolidBackground));
        backdrop.setColor1(QColor(0x33, 0x66, 0x99));
        backdrop.setColor2(QColor(0xFF, 0xFF, 0xFF));
        backdrop.setMotif(int(Motif::Stars));
        backdrop.setMotifInk(Qt::red);
        backdrop.setMotifOpacity(0.8);
        QImage image = paintItem(backdrop);
        QCOMPARE(pixelsNear(image, QColor(0x33, 0x66, 0x99), 0), 300 * 400);

        // Gradient: top to bottom over the item's height, no motif.
        backdrop.setKind(int(Profile::BackgroundKind::GradientBackground));
        backdrop.setColor1(Qt::black);
        backdrop.setColor2(Qt::white);
        image = paintItem(backdrop);
        QVERIFY(near(at(image, 150, 0), Qt::black, 2));
        QVERIFY(near(at(image, 150, 399), Qt::white, 2));
        QVERIFY(near(at(image, 150, 200), QColor(128, 128, 128), 3));
        for (int x = 0; x < 300; x += 7)
            QCOMPARE(at(image, x, 123), at(image, 0, 123));

        // Pattern over a gradient: the base where there is no shape, the ink
        // premixed with the base where there is.
        backdrop.setKind(int(Profile::BackgroundKind::PatternBackground));
        backdrop.setMotifOpacity(0.5);
        image = paintItem(backdrop);
        int inked = 0;
        for (int y = 0; y < 400; y += 2) {
            const QColor row = at(image, 0, y); // (0, y) is never inked by the stars' layout at M
            for (int x = 0; x < 300; x += 2) {
                const QColor pixel = at(image, x, y);
                if (near(pixel, Readability::mix(row, Qt::red, 0.5), 3))
                    ++inked;
            }
        }
        QVERIFY(inked > 200);

        // Picture: the base colour alone (the photo is a scene-graph layer).
        backdrop.setKind(int(Profile::BackgroundKind::ImageBackground));
        backdrop.setColor1(QColor(0x12, 0x34, 0x56));
        image = paintItem(backdrop);
        QCOMPARE(pixelsNear(image, QColor(0x12, 0x34, 0x56), 0), 300 * 400);

        // Cyber grid: the horizon line at 60% of the viewport, the magenta
        // glow band just above it, the grid in the ink below.
        backdrop.setKind(int(Profile::BackgroundKind::PatternBackground));
        backdrop.setMotif(int(Motif::CyberGrid));
        backdrop.setColor1(QColor(0x05, 0x07, 0x16));
        backdrop.setColor2(QColor(0x05, 0x07, 0x16));
        backdrop.setMotifInk(QColor(0x00, 0xE5, 0xFF));
        backdrop.setMotifOpacity(0.4);
        image = paintItem(backdrop);
        const int horizon = qRound(400 * 0.6);
        const QColor line = at(image, 10, horizon);
        QVERIFY2(line.green() > 150 && line.blue() > 150, qPrintable(hex(line)));
        const QColor glow = at(image, 150, horizon - 20);
        QVERIFY2(glow.red() > glow.green() + 20 && glow.blue() > glow.green() + 20, qPrintable(hex(glow)));
        QVERIFY(inkedShare(image.copy(0, horizon + 2, 300, 400 - horizon - 2), QColor(0x05, 0x07, 0x16), 30) > 0.02);

        // Bubbles: ink circles and white ribbons over the base.
        backdrop.setMotif(int(Motif::Bubbles));
        backdrop.setColor1(QColor(0xC7, 0xE1, 0xF5));
        backdrop.setColor2(QColor(0xC7, 0xE1, 0xF5));
        backdrop.setMotifInk(QColor(0x20, 0x60, 0xC0));
        backdrop.setMotifOpacity(0.45);
        image = paintItem(backdrop);
        // (Five bubbles on this small viewport: area / 22 000.)
        QVERIFY(inkedShare(image, QColor(0xC7, 0xE1, 0xF5), 10) > 0.01);

        // Aurora (the stub's backdrop): ribbons only, never a bubble.
        backdrop.setAurora(true);
        image = paintItem(backdrop);
        int lighter = 0;
        for (int y = 0; y < 400; y += 2) {
            for (int x = 0; x < 300; x += 2) {
                const QColor pixel = at(image, x, y);
                QVERIFY2(pixel.blue() >= 0xF5 - 1, qPrintable(hex(pixel))); // no dark-blue ink anywhere
                lighter += pixel.red() > 0xC7 + 3 ? 1 : 0;
            }
        }
        QVERIFY(lighter > 500);
        QVERIFY(backdrop.paintCount() > 0);
    }

    // ------------------------------------------------------------ picture layer

    void imageLayerPlacesFillFitCenterAndTile()
    {
        Stage stage(QSize(400, 300));
        auto *layer = stage.add<ProfileImageLayer>(QSizeF(400, 300));
        QVERIFY(stage.show());
        const QString key = requestAndWait(jpegOf(halves(200, 100, Qt::red, Qt::blue)));
        layer->setImageKey(key);
        QVERIFY(layer->ready());

        const auto grab = [&stage] {
            stage.window.update();
            return stage.window.grabWindow();
        };
        const QColor red(255, 0, 0);
        const QColor blue(0, 0, 255);
        const QColor green(0, 255, 0);

        layer->setImageMode(int(Profile::ImageMode::FillImage)); // cover: 3× → 600×300, cropped
        QImage shot = grab();
        QVERIFY(near(at(shot, 50, 150), red, 40));
        QVERIFY(near(at(shot, 350, 150), blue, 40));
        QVERIFY(near(at(shot, 5, 5), red, 40));
        QVERIFY(near(at(shot, 395, 295), blue, 40));
        QCOMPARE(layer->nodeCount(), 1);

        layer->setImageMode(int(Profile::ImageMode::FitImage)); // contain: 2× → 400×200 at y 50
        shot = grab();
        QVERIFY(near(at(shot, 100, 150), red, 40));
        QVERIFY(near(at(shot, 300, 150), blue, 40));
        QVERIFY(near(at(shot, 200, 20), green, 10));
        QVERIFY(near(at(shot, 200, 285), green, 10));

        layer->setImageMode(int(Profile::ImageMode::CenterImage)); // 1:1 at (100, 100)
        shot = grab();
        QVERIFY(near(at(shot, 130, 150), red, 40));
        QVERIFY(near(at(shot, 270, 150), blue, 40));
        QVERIFY(near(at(shot, 50, 150), green, 10));
        QVERIFY(near(at(shot, 200, 50), green, 10));
        QVERIFY(near(at(shot, 200, 250), green, 10));

        layer->setImageMode(int(Profile::ImageMode::TileImage)); // repeated from the top left
        shot = grab();
        QVERIFY(near(at(shot, 50, 50), red, 40));
        QVERIFY(near(at(shot, 150, 50), blue, 40));
        QVERIFY(near(at(shot, 250, 150), red, 40));
        QVERIFY(near(at(shot, 350, 250), blue, 40));
        QCOMPARE(pixelsNear(shot, green, 10), 0);
        // The 200×100 picture was repeated into one 400×300 block first.
        QCOMPARE(layer->nodeCount(), 1);
        QCOMPARE(layer->textureUploads(), 2); // once plain, once as the tile block
    }

    void imageLayerScrollsWithoutRepainting()
    {
        Stage stage(QSize(400, 300));
        auto *layer = stage.add<ProfileImageLayer>(QSizeF(400, 300));
        QVERIFY(stage.show());
        layer->setImageKey(requestAndWait(jpegOf(halves(200, 100, Qt::red, Qt::blue))));
        layer->setImageMode(int(Profile::ImageMode::CenterImage));
        const auto grab = [&stage] {
            stage.window.update();
            return stage.window.grabWindow();
        };
        QImage shot = grab();
        QVERIFY(near(at(shot, 150, 60), Qt::green, 10));
        QVERIFY(near(at(shot, 150, 170), Qt::red, 40));
        const int uploads = layer->textureUploads();
        QCOMPARE(uploads, 1);

        layer->setScrollOffset(50);
        shot = grab();
        QVERIFY(near(at(shot, 150, 60), Qt::red, 40)); // the picture moved up with the page
        QVERIFY(near(at(shot, 150, 170), Qt::green, 10));
        layer->setScrollOffset(120);
        shot = grab();
        QVERIFY(near(at(shot, 150, 10), Qt::red, 40));
        QVERIFY(near(at(shot, 150, 90), Qt::green, 10));
        QCOMPARE(layer->textureUploads(), uploads);

        // A tiled picture's grid slides and wraps; still no new pixels. The
        // picture is red above and blue below, 100 px tall.
        QImage bands(200, 100, QImage::Format_RGB32);
        bands.fill(Qt::blue);
        QPainter(&bands).fillRect(0, 0, 200, 50, Qt::red);
        layer->setImageKey(requestAndWait(jpegOf(bands)));
        layer->setImageMode(int(Profile::ImageMode::TileImage));
        layer->setScrollOffset(0);
        shot = grab();
        const int tileUploads = layer->textureUploads();
        QVERIFY(near(at(shot, 50, 25), Qt::red, 40));
        QVERIFY(near(at(shot, 50, 75), Qt::blue, 40));
        for (const int offset : {50, 150, 350, 1050}) {
            layer->setScrollOffset(offset); // an odd number of half bands: the colours swap
            shot = grab();
            QVERIFY2(near(at(shot, 50, 25), Qt::blue, 40), qPrintable(QString::number(offset)));
            QVERIFY2(near(at(shot, 350, 275), Qt::red, 40), qPrintable(QString::number(offset)));
        }
        layer->setScrollOffset(100);
        shot = grab();
        QVERIFY(near(at(shot, 50, 25), Qt::red, 40));
        QCOMPARE(layer->textureUploads(), tileUploads);
    }

    // ------------------------------------------------------------ media store

    void mediaStoreDecodesOffTheGuiThread()
    {
        ProfileMediaStore &store = ProfileMediaStore::instance();
        std::atomic<QThread *> decodedOn{nullptr};
        std::atomic_int decodes{0};
        QSemaphore gate(0);
        std::atomic_bool holding{false};
        store.setDecodeHookForTesting([&] {
            decodedOn = QThread::currentThread();
            ++decodes;
            if (holding)
                gate.acquire();
        });

        const QByteArray jpeg = jpegOf(photoLike(640, 400, 1, 10));
        QSignalSpy ready(&store, &ProfileMediaStore::imageReady);
        const QString key = store.requestImage(jpeg);
        QCOMPARE(key, ProfileMediaStore::imageKeyFor(pageMediaHash(jpeg)));
        QVERIFY(key.startsWith(QStringLiteral("pagebg:")));
        QCOMPARE(key.size(), 7 + 64);
        QVERIFY(ready.wait(10000));
        QCOMPARE(ready.takeFirst().at(0).toString(), key);
        QVERIFY(decodedOn.load() != nullptr);
        QVERIFY(decodedOn.load() != QCoreApplication::instance()->thread());
        QCOMPARE(store.image(key)->size(), QSize(640, 400));
        QVERIFY(store.stats(key).has_value());
        // Decoded already: asking again costs nothing.
        QCOMPARE(store.requestImage(jpeg), key);
        QCOMPARE(decodes.load(), 1);

        // While a decode is in flight, another request for it shares it.
        holding = true;
        const QByteArray second = jpegOf(photoLike(320, 200, 2, 10));
        const QString secondKey = store.requestImage(second);
        QCOMPARE(store.requestImage(second), secondKey);
        QVERIFY(!store.image(secondKey));
        gate.release();
        QVERIFY(ready.wait(10000));
        QCOMPARE(ready.size(), 1);
        QCOMPARE(decodes.load(), 2);

        // A release before the decode finishes discards its result.
        const QByteArray third = jpegOf(photoLike(300, 300, 3, 10));
        const QString thirdKey = store.requestImage(third);
        store.release(thirdKey);
        gate.release();
        QThreadPool::globalInstance()->waitForDone(10000);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCOMPARE(ready.size(), 1);
        QVERIFY(!store.image(thirdKey));
        QVERIFY(!store.stats(thirdKey));
        store.setDecodeHookForTesting({});
    }

    void mediaStoreRefusesScanBombsAndOversize()
    {
        ProfileMediaStore &store = ProfileMediaStore::instance();
        std::atomic_int decodes{0};
        store.setDecodeHookForTesting([&] { ++decodes; });
        const QByteArray real = jpegOf(photoLike(200, 120, 4, 6));
        QCOMPARE(jpegScanCount(real), 1);

        const QByteArray bomb = withRepeatedScan(real, 33);
        QCOMPARE(jpegScanCount(bomb), 33);
        QCOMPARE(store.requestImage(bomb), QString());
        const QByteArray mostScans = withRepeatedScan(real, maxJpegScans);
        QCOMPARE(jpegScanCount(mostScans), maxJpegScans);

        QByteArray oversize = real;
        oversize.insert(oversize.size() - 2, QByteArray(maxBackgroundImageBytes, '\0'));
        QVERIFY(oversize.size() > maxBackgroundImageBytes);
        QCOMPARE(store.requestImage(oversize), QString());

        QByteArray png;
        {
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            photoLike(64, 64, 5, 0).save(&buffer, "PNG");
        }
        QCOMPARE(store.requestImage(png), QString());                        // not a JPEG
        QImage wide(2100, 40, QImage::Format_RGB32);
        wide.fill(Qt::darkGreen);
        QCOMPARE(store.requestImage(jpegOf(wide)), QString()); // > 2048 a side
        QCOMPARE(store.requestImage(QByteArray("\xFF\xD8\xFF\xD9", 4)), QString()); // no scan at all
        QCOMPARE(store.requestImage(QByteArray()), QString());
        QCOMPARE(decodes.load(), 0); // nothing refused ever reached a decoder

        QSignalSpy ready(&store, &ProfileMediaStore::imageReady);
        QVERIFY(!store.requestImage(real).isEmpty());
        QVERIFY(ready.wait(10000));
        QCOMPARE(decodes.load(), 1);
        store.setDecodeHookForTesting({});
    }

    void mediaStoreStatsUseTileExtremes()
    {
        // A fine one-pixel black/white checkerboard: averaging would call it
        // grey; the tile extremes see the black and the white text must
        // survive.
        QImage checker(512, 512, QImage::Format_RGB32);
        for (int y = 0; y < 512; ++y) {
            for (int x = 0; x < 512; ++x)
                checker.setPixel(x, y, (x + y) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
        }
        const QString key = requestAndWait(jpegOf(checker, 95));
        const std::optional<ImageStats> stats = ProfileMediaStore::instance().stats(key);
        QVERIFY(stats);
        QVERIFY2(Readability::relativeLuminance(stats->darkest) < 0.03, qPrintable(hex(stats->darkest)));
        QVERIFY2(Readability::relativeLuminance(stats->lightest) > 0.85, qPrintable(hex(stats->lightest)));
        QVERIFY(stats->average.red() > 90 && stats->average.red() < 170);

        // A flat picture: its extremes are its colour.
        QImage flat(300, 200, QImage::Format_RGB32);
        flat.fill(QColor(0x80, 0x40, 0x20));
        const ImageStats plain = ProfileMediaStore::computeStats(flat);
        QVERIFY(near(plain.darkest, QColor(0x80, 0x40, 0x20), 0));
        QVERIFY(near(plain.lightest, QColor(0x80, 0x40, 0x20), 0));
        QVERIFY(near(plain.average, QColor(0x80, 0x40, 0x20), 0));
        // A tiny bright speck does not become "the lightest": percentiles,
        // not the maximum.
        flat.setPixel(150, 100, qRgb(255, 255, 255));
        QVERIFY(near(ProfileMediaStore::computeStats(flat).lightest, QColor(0x80, 0x40, 0x20), 0));
    }

    void mediaStoreLowMemoryKeepsOneDecoded()
    {
        ProfileMediaStore &store = ProfileMediaStore::instance();
        QStringList keys;
        for (int i = 0; i < 5; ++i)
            keys.push_back(requestAndWait(jpegOf(photoLike(160 + i * 8, 100, 10 + i, 4))));
        QCOMPARE(store.decodedCount(), 4);
        QVERIFY(!store.image(keys[0])); // least recently used: its pixels went
        QVERIFY(store.stats(keys[0]));  // its colours stay while it is in use
        for (int i = 1; i < 5; ++i)
            QVERIFY(store.image(keys[i]));

        // Released pictures stay cached (least recently used first out)...
        store.release(keys[4]);
        QVERIFY(store.image(keys[4]));

        // ...until Low memory mode: one decoded picture, freed on release.
        store.setKeepDecoded(false);
        QCOMPARE(store.decodedCount(), 1);
        QVERIFY(store.image(keys[3]));
        const QString next = requestAndWait(jpegOf(photoLike(240, 100, 99, 4)));
        QCOMPARE(store.decodedCount(), 1);
        QVERIFY(store.image(next));
        QVERIFY(!store.image(keys[3]));
        store.release(next);
        QCOMPARE(store.decodedCount(), 0);
        QVERIFY(!store.image(next));
        QVERIFY(!store.stats(next));
    }

    // ------------------------------------------------------------ background pipeline

    void backgroundPipelineFitsBudgetKeepsAspectAndIsBaseline()
    {
        // Grainy and big: must step down the ladder to fit.
        QImage source = photoLike(3000, 2000, 7, 48);
        source.setText(QStringLiteral("Description"), QStringLiteral("private note"));
        std::vector<qreal> progress;
        const auto result = processProfileBackground(source, Qt::white, {}, [&](qreal p) { progress.push_back(p); });
        QVERIFY(result);
        const ProcessedBackground &processed = result.value();
        QVERIFY(processed.jpeg.size() <= maxBackgroundImageBytes);
        QVERIFY(std::max(processed.size.width(), processed.size.height()) <= 1920);
        QVERIFY(std::abs(qreal(processed.size.width()) / processed.size.height() - 1.5) < 0.01);
        QCOMPARE(decodeJpeg(processed.jpeg).size(), processed.size);
        // Baseline (SOF0, one scan), with no metadata segments at all.
        QVERIFY(hasMarker(processed.jpeg, 0xC0));
        QVERIFY(!hasMarker(processed.jpeg, 0xC2));
        QCOMPARE(jpegScanCount(processed.jpeg), 1);
        QVERIFY(!hasMarker(processed.jpeg, 0xE1)); // EXIF
        QVERIFY(!hasMarker(processed.jpeg, 0xE2)); // ICC
        QVERIFY(!hasMarker(processed.jpeg, 0xFE)); // comments (QImage text)
        QVERIFY(processed.jpeg.indexOf("private note") < 0);
        // It had to work for it, and said so.
        QVERIFY(progress.size() >= 2);
        QVERIFY(std::is_sorted(progress.begin(), progress.end()));
        QCOMPARE(progress.back(), 1.0);

        // A smooth photo keeps the full 1920 px; a small one is never enlarged.
        const auto smooth = processProfileBackground(photoLike(2400, 1600, 8, 0), Qt::white);
        QVERIFY(smooth);
        QCOMPARE(smooth.value().size, QSize(1920, 1280));
        const auto small = processProfileBackground(photoLike(400, 300, 9, 0), Qt::white);
        QVERIFY(small);
        QCOMPARE(small.value().size, QSize(400, 300));

        // A budget nothing can meet is refused, not overrun.
        ProfileBackgroundLimits tiny;
        tiny.maxOutputBytes = 300;
        const auto refused = processProfileBackground(photoLike(800, 600, 10, 40), Qt::white, tiny);
        QVERIFY(!refused);
        QCOMPARE(refused.error(), ProfileImageError::EncodeFailed);
    }

    void backgroundPipelineCompositesOntoMatte()
    {
        QImage source(64, 64, QImage::Format_ARGB32);
        source.fill(Qt::transparent);
        {
            QPainter painter(&source);
            painter.fillRect(16, 16, 32, 32, Qt::red);
        }
        const auto result = processProfileBackground(source, QColor(0x00, 0x80, 0xFF));
        QVERIFY(result);
        const QImage decoded = decodeJpeg(result.value().jpeg);
        QCOMPARE(decoded.size(), QSize(64, 64));
        QVERIFY(near(at(decoded, 4, 4), QColor(0x00, 0x80, 0xFF), 12));
        QVERIFY(near(at(decoded, 32, 32), Qt::red, 24));
    }

    void backgroundImporterReportsProgressAndCancels()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString big = directory.filePath(QStringLiteral("big.png"));
        const QString small = directory.filePath(QStringLiteral("small.png"));
        QVERIFY(photoLike(2600, 1700, 11, 48).save(big));
        QVERIFY(photoLike(320, 200, 12, 0).save(small));

        ProfileBackgroundImporter importer;
        QSignalSpy finished(&importer, &ProfileBackgroundImporter::finished);
        QSignalSpy failed(&importer, &ProfileBackgroundImporter::failed);
        QSignalSpy progress(&importer, &ProfileBackgroundImporter::progressChanged);
        importer.start(big, Qt::white);
        QVERIFY(importer.busy());
        QVERIFY(finished.wait(60000));
        QCOMPARE(failed.size(), 0);
        QVERIFY(!importer.busy());
        QCOMPARE(importer.progress(), 1.0);
        QVERIFY(progress.size() >= 2);
        const QByteArray jpeg = finished.at(0).at(0).toByteArray();
        QVERIFY(!jpeg.isEmpty() && jpeg.size() <= maxBackgroundImageBytes);
        QCOMPARE(decodeJpeg(jpeg).size(), finished.at(0).at(1).toSize());

        // A newer start() replaces the older run: only its result arrives.
        finished.clear();
        importer.start(big, Qt::white);
        importer.start(small, Qt::white);
        QVERIFY(finished.wait(60000));
        QThreadPool::globalInstance()->waitForDone(60000);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(1).toSize(), QSize(320, 200));

        // A cancelled run reports nothing at all.
        finished.clear();
        importer.start(big, Qt::white);
        importer.cancel();
        QVERIFY(!importer.busy());
        QCOMPARE(importer.progress(), 0.0);
        QThreadPool::globalInstance()->waitForDone(60000);
        QCoreApplication::processEvents();
        QTest::qWait(50);
        QCOMPARE(finished.size(), 0);
        QCOMPARE(failed.size(), 0);

        // Failures carry the background's own wording.
        importer.start(directory.filePath(QStringLiteral("missing.png")), Qt::white);
        QVERIFY(failed.wait(10000));
        QCOMPARE(failed.at(0).at(0).value<ProfileImageError>(), ProfileImageError::FileMissing);
        QCOMPARE(failed.at(0).at(1).toString(), profileBackgroundErrorText(ProfileImageError::FileMissing));
    }

    void backgroundPipelineRefusesBadFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        using Refusal = std::optional<ProfileImageError>;
        const auto refusal = [](const QString &path, const ProfileBackgroundLimits &limits = {}) {
            const auto result = processProfileBackgroundFile(path, Qt::white, limits);
            return result ? Refusal() : Refusal(result.error());
        };
        QCOMPARE(refusal(directory.filePath(QStringLiteral("nothing.jpg"))), Refusal(ProfileImageError::FileMissing));

        const QString text = directory.filePath(QStringLiteral("notes.png"));
        {
            QFile file(text);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("this is not a picture");
        }
        QCOMPARE(refusal(text), Refusal(ProfileImageError::Unreadable));

        const QString tiny = directory.filePath(QStringLiteral("tiny.png"));
        QVERIFY(photoLike(20, 40, 1, 0).save(tiny));
        QCOMPARE(refusal(tiny), Refusal(ProfileImageError::TooSmall));
        QVERIFY(profileBackgroundErrorText(ProfileImageError::TooSmall).contains(QStringLiteral("32")));

        const QString wide = directory.filePath(QStringLiteral("wide.png"));
        QImage panorama(12'100, 40, QImage::Format_RGB32);
        panorama.fill(Qt::darkCyan);
        QVERIFY(panorama.save(wide));
        QCOMPARE(refusal(wide), Refusal(ProfileImageError::TooLarge));

        const QString fine = directory.filePath(QStringLiteral("fine.png"));
        QVERIFY(photoLike(200, 100, 2, 0).save(fine));
        ProfileBackgroundLimits limits;
        limits.maxFileBytes = 64;
        QCOMPARE(refusal(fine, limits), Refusal(ProfileImageError::FileTooLarge));
        QCOMPARE(refusal(fine), Refusal());
    }

    // ------------------------------------------------------------ readability

    void contrastRatioMatchesWcag()
    {
        QCOMPARE(Readability::relativeLuminance(Qt::white), 1.0);
        QCOMPARE(Readability::relativeLuminance(Qt::black), 0.0);
        QVERIFY(std::abs(Readability::contrastRatio(Qt::black, Qt::white) - 21.0) < 1e-9);
        QVERIFY(std::abs(Readability::contrastRatio(QColor(0x76, 0x76, 0x76), Qt::white) - 4.54) < 0.01);
        QCOMPARE(Readability::contrastRatio(QColor(0x76, 0x76, 0x76), Qt::white),
                 Readability::contrastRatio(Qt::white, QColor(0x76, 0x76, 0x76)));
        QCOMPARE(Readability::contrastRatio(QColor(0x33, 0x66, 0x99), QColor(0x33, 0x66, 0x99)), 1.0);
        // Mixing rounds to 8-bit channels, as the reference does.
        QCOMPARE(Readability::mix(Qt::black, Qt::white, 0.5), QColor(128, 128, 128));
        QCOMPARE(Readability::lighten(QColor(0x9F, 0xCD, 0xEF), 0.34), QColor(0xC0, 0xDE, 0xF4));
        QCOMPARE(Readability::over(QColor(255, 255, 255, 128), Qt::black), QColor(128, 128, 128));
    }

    void presetContrastMatchesTheDesignTable_data()
    {
        QTest::addColumn<int>("preset");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("opacity");
        // body, label, link, secondary, strip, alt strip, cell label, cell value, name, online (-1: n/a)
        QTest::addColumn<QVector<double>>("expected");
        const auto row = [](const char *name, Profile::Preset preset, bool dark, int opacity,
                            const QVector<double> &values) {
            QTest::newRow(name) << int(preset) << dark << opacity << values;
        };
        using P = Profile::Preset;
        row("Aero Sky light", P::AeroSkyPreset, false, 88, {10.80, 8.26, 5.18, 4.56, 6.09, -1, 6.46, 9.74, 10.55, 4.89});
        row("Aero Sky dark", P::AeroSkyPreset, true, 90, {11.58, 8.04, 6.95, 6.17, 4.82, -1, 6.60, 10.83, 13.18, 8.28});
        row("Classic '06", P::Classic06Preset, false, 100, {21.00, 8.65, 10.86, 7.81, 4.86, 4.78, 5.81, 17.71, 21.00, 5.13});
        row("Scene Queen", P::SceneQueenPreset, false, 94, {17.78, 7.65, 7.65, 8.03, 6.36, -1, -1, -1, 6.92, 11.47});
        row("Neon Zebra", P::NeonZebraPreset, false, 92, {17.56, 12.78, 7.24, 8.08, 10.43, -1, -1, -1, 16.19, 11.53});
        row("Midnight Emo", P::MidnightEmoPreset, false, 94, {12.89, 6.17, 7.92, 6.47, 5.13, -1, -1, -1, 15.40, 10.65});
        row("Glitter Girl", P::GlitterGirlPreset, false, 94, {14.21, 6.19, 6.19, 5.39, 5.40, -1, 4.64, 12.23, 3.30, 4.90});
        row("Safety Pin", P::SafetyPinPreset, false, 94, {16.13, 13.19, 13.19, 7.76, 6.95, -1, -1, -1, 18.88, 11.08});
        row("Headliner", P::HeadlinerPreset, false, 94, {15.00, 11.42, 9.89, 7.35, 6.26, -1, -1, -1, 11.42, 10.92});
        row("Linen", P::LinenPreset, false, 97, {14.13, 6.25, 6.25, 5.03, 12.20, -1, -1, -1, 14.13, 5.08});
        row("Chrome Y2K", P::ChromeY2KPreset, false, 88, {14.45, 12.25, 14.21, 6.97, 10.48, -1, -1, -1, -1, 10.05});
    }

    void presetContrastMatchesTheDesignTable()
    {
        QFETCH(int, preset);
        QFETCH(bool, dark);
        QFETCH(int, opacity);
        QFETCH(QVector<double>, expected);
        Profile::Theme theme = Profile::presetTheme(Profile::Preset(preset));
        if (theme.adaptive)
            theme = Profile::aeroSkyTheme(dark);
        const Readability::Palette palette = Readability::resolve(theme, Readability::pageSamples(theme, std::nullopt));
        QCOMPARE(palette.resolvedOpacity, opacity);
        QVERIFY2(!palette.adjusted, palette.adjustments.isEmpty() ? "" : qPrintable(palette.adjustments[0].sentence));
        QVERIFY(palette.inkAdjusted.isEmpty());
        QVERIFY(!palette.halo);
        QCOMPARE(palette.boxFill.alpha(), qRound(opacity * 255 / 100.0));
        const QVector<QColor> &boxes = palette.boxBackgrounds;
        const auto check = [](double measured, double wanted, const char *what) {
            if (wanted < 0)
                return;
            QVERIFY2(std::abs(measured - wanted) <= 0.02,
                     qPrintable(QStringLiteral("%1: %2, SPEC says %3").arg(QLatin1String(what)).arg(measured).arg(wanted)));
        };
        check(minContrast(palette.body, boxes), expected[0], "body");
        check(minContrast(palette.label, boxes), expected[1], "label");
        check(minContrast(palette.link, boxes), expected[2], "link");
        check(minContrast(palette.muted, boxes), expected[3], "secondary");
        const double strip = theme.headerStyle == Profile::HeaderStyle::NoHeader
                                 ? minContrast(palette.headerText, boxes)
                                 : minContrast(palette.headerText, stops(palette.strip));
        check(strip, expected[4], "strip title");
        check(minContrast(palette.altHeaderText, stops(palette.altStrip)), expected[5], "alt strip title");
        check(Readability::contrastRatio(palette.cellLabelInk, palette.cellLabelFill), expected[6], "cell label");
        check(Readability::contrastRatio(palette.cellValueInk, palette.cellValueFill), expected[7], "cell value");
        check(minContrast(palette.name, boxes), expected[8], "name");
        check(minContrast(palette.presence[0], boxes), expected[9], "Online Now!");
        // Every renderer-derived ink holds its floor too.
        for (const QColor &presence : palette.presence)
            QVERIFY(minContrast(presence, boxes) >= 4.5);
        QVERIFY(Readability::contrastRatio(palette.monogramInk, palette.monogramTop) >= 3.0);
        QVERIFY(Readability::contrastRatio(palette.monogramInk, palette.monogramBottom) >= 3.0);
        QCOMPARE(palette.songMaterial, palette.boxDark ? 1 : 0);
    }

    void stripRecipeMatchesTheSpec()
    {
        using Profile::HeaderStyle;
        // Expected values from the reference (Color.js headerStops).
        struct Case final {
            const char *fill;
            HeaderStyle style;
            bool dark;
            const char *stops[4];
            const char *line;
            int highlightAlpha;
        };
        const Case cases[] = {
            {"#9fcdef", HeaderStyle::GlossHeader, false, {"#c0def4", "#aed5f2", "#9fcdef", "#95c1e1"}, "#80a6c2", 176},
            {"#355871", HeaderStyle::GlossHeader, true, {"#4d6c82", "#3f6078", "#355871", "#2f4d63"}, "#1f3240", 56},
            {"#9fcdef", HeaderStyle::GradientHeader, false, {"#b2d7f2", "#a1c8e5", "#a1c8e5", "#8fb9d7"}, "#7b9fb9", 112},
            {"#3a0e16", HeaderStyle::GradientHeader, true, {"#4e262d", "#411a21", "#411a21", "#340d14"}, "#22080d", 33},
            {"#4574a8", HeaderStyle::FlatHeader, true, {"#4574a8", "#4574a8", "#4574a8", "#4574a8"}, "#2d4b6d", 0},
            {"#b3121e", HeaderStyle::FlatHeader, true, {"#b3121e", "#b3121e", "#b3121e", "#b3121e"}, "#740c14", 0},
        };
        for (const Case &c : cases) {
            const Readability::StripRecipe recipe = Readability::stripRecipe(QColor(QLatin1String(c.fill)), c.style);
            QCOMPARE(recipe.dark, c.dark);
            for (int i = 0; i < 4; ++i)
                QCOMPARE(recipe.stops[std::size_t(i)].name(), QLatin1String(c.stops[i]));
            QCOMPARE(recipe.bottomLine.name(), QLatin1String(c.line));
            QCOMPARE(recipe.highlight.alpha(), c.highlightAlpha);
            QCOMPARE(recipe.positions, (std::array<qreal, 4>{0.0, 0.49, 0.50, 1.0}));
        }
        // The default strip is the SPEC's sky blue with its navy title.
        const Readability::Palette aero = Readability::resolve(Profile::aeroSkyTheme(false),
                                                               Readability::pageSamples(Profile::aeroSkyTheme(false), {}));
        QCOMPARE(aero.strip.stops[2].name(), QStringLiteral("#9fcdef"));
        QCOMPARE(aero.headerText.name(), QStringLiteral("#133a61"));
    }

    void unreadableThemeIsCorrectedInOrder()
    {
        using Profile::EditorTab;
        using Profile::InkRole;
        // A see-through box over a heavy pattern, a faint link, white strip
        // text on a mid-grey strip: every correction step has work to do.
        Profile::Theme theme = Profile::aeroSkyTheme(false);
        theme.backgroundKind = Profile::BackgroundKind::PatternBackground;
        theme.backgroundColor1 = theme.backgroundColor2 = 0xFFFFFF;
        theme.motif = Motif::Stars;
        theme.motifInk = 0x000000;
        theme.motifOpacity = 80;
        theme.boxFill = 0xFFFFFF;
        theme.boxOpacity = 60;
        theme.linkColor = 0xDDDDDD;
        theme.headerStyle = Profile::HeaderStyle::FlatHeader;
        theme.headerFill = 0x808080;
        theme.headerText = 0xFFFFFF;
        const QVector<QColor> samples = Readability::pageSamples(theme, std::nullopt);
        const Readability::Palette palette = Readability::resolve(theme, samples);

        // 1. Opacity first, until everything but the hopeless link passes.
        QCOMPARE(palette.resolvedOpacity, 100);
        QVERIFY(palette.adjusted);
        QCOMPARE(palette.adjustments.size(), 3);
        QCOMPARE(palette.adjustments[0].role, -1);
        QCOMPARE(palette.adjustments[0].tab, EditorTab::BackgroundTab);
        QVERIFY(palette.adjustments[0].sentence.startsWith(QStringLiteral("Boxes are drawn 100% solid (you chose 60%)")));
        // 2. Then the ink that still fails moves, darker here.
        QCOMPARE(palette.adjustments[1].role, int(InkRole::LinkInk));
        QCOMPARE(palette.adjustments[1].tab, EditorTab::TextTab);
        QVERIFY(palette.adjustments[1].sentence.contains(QStringLiteral("link colour")));
        QVERIFY(palette.adjustments[1].sentence.contains(QStringLiteral("darker")));
        QCOMPARE(palette.link.name(), QStringLiteral("#737373")); // the reference engine's answer
        QVERIFY(palette.inkAdjusted.value(int(InkRole::LinkInk)));
        QCOMPARE(palette.body, rgb(theme.bodyColor)); // untouched inks stay exact
        QVERIFY(!palette.inkAdjusted.contains(int(InkRole::BodyInk)));
        // 3. The strip fill moves before its title does: deepening alone is
        //    enough here, so the owner's white title stays exact.
        QCOMPARE(palette.adjustments[2].role, -1);
        QCOMPARE(palette.adjustments[2].tab, EditorTab::BoxesTab);
        QVERIFY(palette.adjustments[2].sentence.contains(QStringLiteral("strip deepened")));
        QCOMPARE(palette.strip.stops[0].name(), QStringLiteral("#767676")); // 8% darker: the first that passes
        QCOMPARE(palette.headerText, QColor(Qt::white));
        QVERIFY(!palette.inkAdjusted.contains(int(InkRole::HeaderTextInk)));
        QVERIFY(minContrast(palette.headerText, stops(palette.strip)) >= 4.5);
        QVERIFY(!palette.halo);

        // Past 48% the title itself shifts.
        Profile::Theme hopeless = Profile::aeroSkyTheme(false);
        hopeless.headerStyle = Profile::HeaderStyle::FlatHeader;
        hopeless.headerFill = 0x808080;
        hopeless.headerText = 0x8A8A8A;
        const Readability::Palette shifted = Readability::resolve(hopeless, Readability::pageSamples(hopeless, {}));
        QCOMPARE(shifted.strip.stops[0].name(), QStringLiteral("#434343"));
        QCOMPARE(shifted.headerText.name(), QStringLiteral("#afafaf")); // the reference engine's answer
        QVERIFY(shifted.inkAdjusted.value(int(InkRole::HeaderTextInk)));
        QCOMPARE(shifted.adjustments.last().role, int(InkRole::HeaderTextInk));

        // 4. Last resort: no colour can pass, so the best one gets a halo in
        //    the opposite pole.
        Readability::Floors impossible;
        impossible.body = 22; // beyond 21:1
        const Readability::Palette haloed = Readability::resolve(Profile::aeroSkyTheme(false),
                                                                 Readability::pageSamples(Profile::aeroSkyTheme(false), {}),
                                                                 impossible);
        QVERIFY(haloed.halo);
        QCOMPARE(haloed.haloColor.alpha(), qRound(0.7 * 255));
        QCOMPARE(haloed.body, QColor(Qt::black)); // the best pole on light boxes
        QCOMPARE(QColor(haloed.haloColor.rgb()), QColor(Qt::white));
        QCOMPARE(haloed.adjustments.last().role, -1);
        QCOMPARE(haloed.adjustments.last().tab, EditorTab::BackgroundTab);
    }

    void imageSamplesDriveCorrection()
    {
        Profile::Theme theme = Profile::aeroSkyTheme(false);
        theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        theme.backgroundColor1 = 0xFFFFFF;
        theme.backgroundColor2 = 0x000000; // unused: a picture sits on its first colour
        theme.boxFill = 0xFFFFFF;
        theme.boxOpacity = 60;
        theme.bodyColor = 0x555555;

        // Until the picture is decoded, only the base colour counts.
        const QVector<QColor> before = Readability::pageSamples(theme, std::nullopt);
        QCOMPARE(before, (QVector<QColor>{QColor(Qt::white)}));
        QCOMPARE(Readability::resolve(theme, before).resolvedOpacity, 60);

        // A photo with deep shadows: the box must be drawn more solid.
        const ImageStats stats{QColor(0x08, 0x08, 0x10), QColor(0xF0, 0xF0, 0xE8), QColor(0x80, 0x80, 0x80)};
        const QVector<QColor> after = Readability::pageSamples(theme, stats);
        QCOMPARE(after, (QVector<QColor>{QColor(Qt::white), stats.darkest, stats.lightest}));
        const Readability::Palette palette = Readability::resolve(theme, after);
        QVERIFY(palette.resolvedOpacity > 60);
        QVERIFY(minContrast(palette.body, palette.boxBackgrounds) >= 4.5);
        QCOMPARE(palette.body, QColor(0x55, 0x55, 0x55));
        QCOMPARE(palette.adjustments.first().tab, Profile::EditorTab::BackgroundTab);

        // Pattern samples: the ink over both gradient stops and the declared
        // highlight inks (skulls: two).
        Profile::Theme scene = Profile::presetTheme(Profile::Preset::SceneQueenPreset);
        QCOMPARE(Readability::pageSamples(scene, {}).size(), 1 + 1 + 2);
        scene.backgroundColor2 = 0x202020;
        QCOMPARE(Readability::pageSamples(scene, {}).size(), 3 + 2 + 2);
    }

    void adjustmentsNameTheirTab()
    {
        using Profile::EditorTab;
        using Profile::InkRole;
        // A see-through box over a plain dark base: the box is to blame.
        Profile::Theme theme = Profile::aeroSkyTheme(false);
        theme.backgroundKind = Profile::BackgroundKind::SolidBackground;
        theme.backgroundColor1 = 0x000000;
        theme.boxOpacity = 60;
        theme.bodyColor = 0x555555;
        Readability::Palette palette = Readability::resolve(theme, Readability::pageSamples(theme, {}));
        QCOMPARE(palette.resolvedOpacity, 92); // the reference engine's answer
        QCOMPARE(palette.adjustments.first().tab, EditorTab::BoxesTab);

        // The same box over a pattern on a light base: the pattern is.
        theme.backgroundKind = Profile::BackgroundKind::PatternBackground;
        theme.backgroundColor1 = theme.backgroundColor2 = 0xFFFFFF;
        theme.motif = Motif::Checkerboard;
        theme.motifInk = 0x000000;
        theme.motifOpacity = 80;
        palette = Readability::resolve(theme, Readability::pageSamples(theme, {}));
        QVERIFY(palette.resolvedOpacity > 60);
        QCOMPARE(palette.adjustments.first().tab, EditorTab::BackgroundTab);

        // Inks belong to the Text tab, the name to Name & FX, strip text to
        // Boxes.
        Profile::Theme inks = Profile::aeroSkyTheme(false);
        inks.boxOpacity = 100;
        inks.bodyColor = 0xEEEEEE;
        inks.labelColor = 0xE0E0E0;
        inks.nameColor = 0xF4F4F4;
        inks.headerStyle = Profile::HeaderStyle::NoHeader;
        inks.headerText = 0xFAFAFA;
        palette = Readability::resolve(inks, Readability::pageSamples(inks, {}));
        const auto tabOf = [&palette](InkRole role) {
            for (const Readability::Adjustment &adjustment : palette.adjustments) {
                if (adjustment.role == int(role))
                    return int(adjustment.tab);
            }
            return -1;
        };
        QCOMPARE(tabOf(InkRole::BodyInk), int(EditorTab::TextTab));
        QCOMPARE(tabOf(InkRole::LabelInk), int(EditorTab::TextTab));
        QCOMPARE(tabOf(InkRole::NameInk), int(EditorTab::NameTab));
        QCOMPARE(tabOf(InkRole::HeaderTextInk), int(EditorTab::BoxesTab));
        QCOMPARE(tabOf(InkRole::LinkInk), -1); // the link was fine
        for (const Readability::Adjustment &adjustment : palette.adjustments)
            QVERIFY(!adjustment.sentence.isEmpty());
        QVERIFY(palette.inkAdjusted.value(int(InkRole::NameInk)));
        // Chrome names are exempt: their dark rim carries them.
        inks.nameEffect = Profile::NameEffect::ChromeName;
        palette = Readability::resolve(inks, Readability::pageSamples(inks, {}));
        QCOMPARE(tabOf(InkRole::NameInk), -1);
        QCOMPARE(palette.name, QColor(0xF4, 0xF4, 0xF4));
    }

    void contrastForReportsThePickerLine()
    {
        const Profile::Theme aero = Profile::aeroSkyTheme(false);
        const QVector<QColor> samples = Readability::pageSamples(aero, {});
        const Readability::ContrastReport good =
            Readability::contrastFor(aero, samples, Profile::InkRole::LinkInk, QColor(0x1F, 0x6F, 0xA3));
        QVERIFY(std::abs(good.ratio - 5.18) <= 0.02);
        QVERIFY(good.passes);
        QCOMPARE(good.shown, QColor(0x1F, 0x6F, 0xA3));

        // Too faint: the line reports the pick's own ratio and what viewers
        // will see instead (the reference: #5c778b on solid boxes).
        const Readability::ContrastReport faint =
            Readability::contrastFor(aero, samples, Profile::InkRole::LinkInk, QColor(0x9F, 0xCD, 0xEF));
        QVERIFY(faint.ratio < 1.8);
        QVERIFY(!faint.passes);
        QCOMPARE(faint.shown.name(), QStringLiteral("#5c778b"));

        // A strip title is measured on the owner's own strip; what viewers
        // see clears 4.5:1 on the strip as the renderer draws it.
        const QColor pale(0x88, 0xAA, 0xCC);
        const Readability::ContrastReport title =
            Readability::contrastFor(aero, samples, Profile::InkRole::HeaderTextInk, pale);
        QVERIFY(!title.passes);
        QVERIFY(std::abs(title.ratio - 1.27) <= 0.01);
        QCOMPARE(title.shown.name(), QStringLiteral("#495c6e")); // on a strip lightened to #cde5f7
        Profile::Theme paleTitle = aero;
        paleTitle.headerText = 0x88AACC;
        const Readability::Palette paleStrip = Readability::resolve(paleTitle, samples);
        QCOMPARE(paleStrip.strip.stops[2].name(), QStringLiteral("#cde5f7"));
        QVERIFY(minContrast(title.shown, stops(paleStrip.strip)) >= 4.5);
        QVERIFY(!paleStrip.halo);
        // A title no colour can carry on a glossy strip, even at 48%: the
        // pole with the better worst case (the reference simply takes black,
        // at 3.11:1), haloed in the other pole.
        paleTitle.headerText = 0xAACCEE;
        const Readability::Palette hopeless = Readability::resolve(paleTitle, samples);
        QVERIFY(hopeless.halo);
        QCOMPARE(hopeless.headerText, QColor(Qt::white));
        const QVector<QColor> hopelessStops = stops(hopeless.strip);
        QVERIFY(minContrast(Qt::white, hopelessStops) < 4.5);
        QVERIFY(minContrast(Qt::white, hopelessStops) > minContrast(Qt::black, hopelessStops));
        QCOMPARE(QColor(hopeless.haloColor.rgb()), QColor(Qt::black));

        // The name's floor is 3:1: a colour too faint for a link is fine as
        // a name (the reference measures #5f86a8 at 3.66 on these boxes).
        const QColor mid(0x5F, 0x86, 0xA8);
        const Readability::ContrastReport name = Readability::contrastFor(aero, samples, Profile::InkRole::NameInk, mid);
        QVERIFY(std::abs(name.ratio - 3.66) <= 0.01);
        QVERIFY(name.passes);
        QCOMPARE(name.shown, mid);
        const Readability::ContrastReport link = Readability::contrastFor(aero, samples, Profile::InkRole::LinkInk, mid);
        QVERIFY(!link.passes);
        QVERIFY(link.shown != mid);
        Profile::Theme chrome = Profile::presetTheme(Profile::Preset::ChromeY2KPreset);
        const Readability::ContrastReport steel = Readability::contrastFor(
            chrome, Readability::pageSamples(chrome, {}), Profile::InkRole::NameInk, QColor(0x10, 0x10, 0x30));
        QVERIFY(steel.passes);
        QVERIFY(steel.ratio < 3.0);
    }

    // ------------------------------------------------------------ the name

    void nameEffectsRender()
    {
        ProfileFonts::ensureRegistered();
        QList<QImage> frames;
        for (int effect = 0; effect <= int(Profile::NameEffect::ShadowName); ++effect) {
            ProfileNameText name;
            name.setText(QStringLiteral("Michael"));
            name.setBasePixelSize(34);
            name.setColor(QColor(0x1C, 0x3D, 0x63));
            name.setColor2(QColor(0xFF, 0x90, 0x10));
            name.setEffect(effect);
            name.setAnimate(false);
            name.setSize(QSizeF(name.implicitWidth(), name.implicitHeight()));
            const QImage frame = name.renderFrame(1.0);
            QVERIFY2(opaquePixels(frame, 128) > 300, qPrintable(QString::number(effect)));
            // It paints nothing outside itself, and the glyphs start at
            // glyphLeft (the overhang is for glows and outlines).
            QCOMPARE(frame.size(), QSize(qCeil(name.width()), qCeil(name.height())));
            QCOMPARE(name.glyphLeft(), std::ceil(name.renderedPixelSize() * 0.36));
            QCOMPARE(name.glyphTop(), std::ceil(name.renderedPixelSize() * 0.26));
            frames.push_back(frame);
            capture(QStringLiteral("name-%1").arg(effectSlug(effect)), onBox(frame, QColor(0xF4, 0xF8, 0xFC)));
        }
        for (int a = 0; a < frames.size(); ++a) {
            for (int b = a + 1; b < frames.size(); ++b)
                QVERIFY2(differingPixels(frames[a], frames[b]) > 60, qPrintable(QStringLiteral("%1 vs %2").arg(a).arg(b)));
        }
        if (!qEnvironmentVariableIsEmpty("OPENCHAT_PROFILE_CAPTURES"))
            captureNamePresets();
    }

    void nameShrinksThenElides()
    {
        ProfileNameText name;
        name.setText(QStringLiteral("Michael"));
        name.setBasePixelSize(34);
        name.setEffect(int(Profile::NameEffect::PlainName));
        const qreal natural = name.textWidth();
        QCOMPARE(name.renderedPixelSize(), 34);
        QVERIFY(!name.elided());

        name.setAvailableWidth(natural + 1);
        QCOMPARE(name.renderedPixelSize(), 34);
        name.setAvailableWidth(natural * 0.8);
        QVERIFY(name.renderedPixelSize() < 34);
        QVERIFY(name.renderedPixelSize() >= 24); // max(20, 0.7 × 34)
        QVERIFY(!name.elided());
        QVERIFY(name.textWidth() <= natural * 0.8);
        QCOMPARE(name.paintedText(), QStringLiteral("Michael"));

        // Past the floor it elides, never shrinking further.
        name.setText(QStringLiteral("Bartholomew Montgomery-Smythe"));
        name.setAvailableWidth(120);
        QCOMPARE(name.renderedPixelSize(), 24);
        QVERIFY(name.elided());
        QVERIFY(name.paintedText().endsWith(QChar(0x2026)));
        QVERIFY(name.textWidth() <= 120);
        QCOMPARE(name.accessibleName(), QStringLiteral("Bartholomew Montgomery-Smythe"));
        // The item is the text plus its overhang on each side.
        QCOMPARE(name.implicitWidth(), std::ceil(name.textWidth()) + 2 * name.glyphLeft());

        // An explicit floor wins over the default.
        name.setMinPixelSize(28);
        QCOMPARE(name.renderedPixelSize(), 28);
    }

    void fancyNamesNeverRenderBelow30()
    {
        ProfileFonts::ensureRegistered();
        ProfileNameText name;
        name.setText(QStringLiteral("Sarah"));
        // An M script name (28 × 0.95 = 27) renders at 30.
        name.setFontFamily(ProfileFonts::family(Profile::Font::ScriptFont, ProfileFonts::Role::Name));
        name.setBasePixelSize(ProfileFonts::namePixelSize(Profile::Font::ScriptFont, Profile::NameSize::MediumName));
        QCOMPARE(name.basePixelSize(), 27);
        QCOMPARE(name.renderedPixelSize(), 30);
        // However little room there is.
        name.setText(QStringLiteral("Sarah Elizabeth Montgomery"));
        name.setAvailableWidth(90);
        QCOMPARE(name.renderedPixelSize(), 30);
        QVERIFY(name.elided());
        // Gothic, and Glitter in any face, too, even with a lower floor asked.
        name.setFontFamily(ProfileFonts::family(Profile::Font::GothicFont, ProfileFonts::Role::Name));
        name.setBasePixelSize(50);
        QCOMPARE(name.renderedPixelSize(), 30);
        name.setFontFamily(QString());
        name.setEffect(int(Profile::NameEffect::GlitterName));
        name.setMinPixelSize(20);
        QCOMPARE(name.renderedPixelSize(), 30);
        // A plain interface name may go down to 20.
        name.setEffect(int(Profile::NameEffect::PlainName));
        name.setMinPixelSize(-1);
        name.setBasePixelSize(28);
        QCOMPARE(name.renderedPixelSize(), 20);
    }

    void pixelNamesSnapToTheGrid()
    {
        ProfileFonts::ensureRegistered();
        QCOMPARE(ProfileFonts::namePixelSize(Profile::Font::PixelFont, Profile::NameSize::MediumName), 16);
        QCOMPARE(ProfileFonts::namePixelSize(Profile::Font::PixelFont, Profile::NameSize::LargeName), 24);
        QCOMPARE(ProfileFonts::namePixelSize(Profile::Font::PixelFont, Profile::NameSize::ExtraLargeName), 32);
        QCOMPARE(ProfileFonts::pixelGrid(Profile::Font::PixelFont), 8);
        QCOMPARE(ProfileFonts::namePixelSize(Profile::Font::FutureFont, Profile::NameSize::ExtraLargeName), 38);

        ProfileNameText name;
        name.setFontFamily(QStringLiteral("Press Start 2P"));
        name.setText(QStringLiteral("Pixel Pete"));
        name.setBasePixelSize(26); // off the grid: snaps down
        QCOMPARE(name.renderedPixelSize(), 24);
        name.setBasePixelSize(32);
        QCOMPARE(name.renderedPixelSize(), 32);
        const qreal natural = name.textWidth();
        name.setAvailableWidth(natural * 0.9);
        QCOMPARE(name.renderedPixelSize(), 24); // one whole grid step, not 29
        QVERIFY(!name.elided());
        name.setAvailableWidth(natural * 0.2);
        QCOMPARE(name.renderedPixelSize() % 8, 0);
        QCOMPARE(name.renderedPixelSize(), 16); // floor: 0.7 × 32 on the grid
        QVERIFY(name.elided());
        // A glittering Pixel name is fancy: at least 30 px, on the grid.
        name.setAvailableWidth(0);
        name.setBasePixelSize(16);
        QCOMPARE(name.renderedPixelSize(), 16);
        name.setEffect(int(Profile::NameEffect::GlitterName));
        QCOMPARE(name.renderedPixelSize(), 32);
        name.setAvailableWidth(natural * 0.2);
        QCOMPARE(name.renderedPixelSize(), 32);
        QVERIFY(name.elided());
    }

    void flourishIsPaintedButNotInTheAccessibleName()
    {
        ProfileFonts::ensureRegistered();
        ProfileNameText name;
        name.setText(QStringLiteral("Jessica"));
        name.setEffect(int(Profile::NameEffect::PlainName));
        name.setAnimate(false);
        const qreal plainWidth = name.implicitWidth();
        name.setSize(QSizeF(name.implicitWidth(), name.implicitHeight()));
        const int plainInk = opaquePixels(name.renderFrame(1.0), 128);

        name.setFlourish(int(Profile::Flourish::XxxFlourish));
        QCOMPARE(name.paintedText(), QStringLiteral("xXx Jessica xXx"));
        QCOMPARE(name.accessibleName(), QStringLiteral("Jessica"));
        QCOMPARE(name.property("accessibleName").toString(), QStringLiteral("Jessica"));
        QCOMPARE(name.text(), QStringLiteral("Jessica"));
        QVERIFY(name.implicitWidth() > plainWidth + 40);
        name.setSize(QSizeF(name.implicitWidth(), name.implicitHeight()));
        QVERIFY(opaquePixels(name.renderFrame(1.0), 128) > plainInk * 1.4);

        // Glyphs the face lacks (★ in Fredoka) still paint, from a fallback.
        name.setFontFamily(ProfileFonts::family(Profile::Font::RoundedFont, ProfileFonts::Role::Name));
        name.setFlourish(int(Profile::Flourish::StarFlourish));
        QCOMPARE(name.paintedText(), QStringLiteral("★ Jessica ★"));
        name.setSize(QSizeF(name.implicitWidth(), name.implicitHeight()));
        const QImage stars = name.renderFrame(1.0);
        const int starColumn = qRound(name.glyphLeft() + name.renderedPixelSize() * 0.3);
        int starInk = 0;
        for (int y = 0; y < stars.height(); ++y)
            starInk += qAlpha(stars.pixel(starColumn, y)) > 128 ? 1 : 0;
        QVERIFY(starInk > 3);
        QCOMPARE(name.accessibleName(), QStringLiteral("Jessica"));
    }

    void nameCacheHoldsOnlyTheStaticFrame()
    {
        ProfileNameText::clearFrameCache();
        ProfileNameText name;
        name.setText(QStringLiteral("Glitter"));
        name.setEffect(int(Profile::NameEffect::GlitterName));
        name.setAnimate(false);
        name.setSize(QSizeF(name.implicitWidth(), name.implicitHeight()));
        const QImage still = name.renderFrame(1.0);
        QCOMPARE(ProfileNameText::cachedFrameCount(), 1);

        name.setPhase(3);
        const QImage three = name.renderFrame(1.0);
        name.setPhase(4);
        const QImage four = name.renderFrame(1.0);
        QCOMPARE(ProfileNameText::cachedFrameCount(), 1); // animated frames are painted live
        QVERIFY(differingPixels(still, three) > 5);       // the grain is reseeded per frame
        QVERIFY(differingPixels(three, four) > 5);
        name.setPhase(3);
        QCOMPARE(name.renderFrame(1.0), three); // deterministic

        name.setPhase(0);
        QCOMPARE(name.renderFrame(1.0), still);
        QCOMPARE(ProfileNameText::cachedFrameCount(), 1); // a cache hit, not a second entry
        name.setColor(Qt::red);
        (void)name.renderFrame(1.0);
        QCOMPARE(ProfileNameText::cachedFrameCount(), 2);
        // Non-glitter effects ignore the phase.
        name.setEffect(int(Profile::NameEffect::GlowName));
        const QImage glow = name.renderFrame(1.0);
        name.setPhase(5);
        QCOMPARE(name.renderFrame(1.0), glow);
    }

    void glitterAnimatesOnlyWhenAllowed()
    {
        Stage stage(QSize(360, 120));
        auto *name = stage.add<ProfileNameText>(QSizeF());
        name->setText(QStringLiteral("Sparkle"));
        name->setEffect(int(Profile::NameEffect::GlitterName));
        QVERIFY(!name->animating()); // not on screen yet
        QVERIFY(stage.show());
        QTRY_VERIFY(name->animating());
        QTRY_VERIFY(name->phase() > 1);
        QVERIFY(ProfileTicker::instance().isRunning());

        ProfileRenderPolicy::instance().setLowMemoryMode(true);
        QVERIFY(!name->animating());
        QCOMPARE(name->phase(), 0); // stopped means the still frame
        QVERIFY(!ProfileTicker::instance().isRunning());
        QTest::qWait(150);
        QCOMPARE(name->phase(), 0);
        ProfileRenderPolicy::instance().setLowMemoryMode(false);
        QTRY_VERIFY(name->phase() > 0);

        ProfileRenderPolicy::instance().setReducedMotion(true);
        QCOMPARE(name->phase(), 0);
        QVERIFY(!name->animating());
        ProfileRenderPolicy::instance().setReducedMotion(false);
        QVERIFY(name->animating());

        name->setAnimate(false);
        QVERIFY(!name->animating());
        QCOMPARE(name->phase(), 0);
        name->setAnimate(true);
        QVERIFY(name->animating());
        name->setEffect(int(Profile::NameEffect::GlowName)); // only glitter moves
        QVERIFY(!name->animating());
        name->setEffect(int(Profile::NameEffect::GlitterName));
        name->setVisible(false);
        QVERIFY(!name->animating());
        name->setVisible(true);
        QVERIFY(name->animating());
        stage.window.hide();
        QVERIFY(!name->animating());
        QCOMPARE(ProfileTicker::instance().subscriberCount(), 0);
    }

    // ------------------------------------------------------------ ambient and ticker

    void ambientRunsOnlyWhileVisibleAndAllowed()
    {
        QCOMPARE(ProfileAmbient::densityFor(QSizeF(400, 300)), 8);
        QCOMPARE(ProfileAmbient::densityFor(QSizeF(860, 680)), 15);
        QCOMPARE(ProfileAmbient::densityFor(QSizeF(1920, 1080)), 28);

        Stage stage(QSize(420, 320));
        auto *ambient = stage.add<ProfileAmbient>(QSizeF(420, 320));
        QCOMPARE(ambient->spriteCount(), 0); // no kind, no sprites
        ambient->setKind(int(Profile::Ambient::FallingSnow));
        QCOMPARE(ambient->spriteCount(), 8);
        QVERIFY(!ambient->animating()); // the window is not shown yet
        QVERIFY(stage.show());
        QTRY_VERIFY(ambient->animating());

        // Held still: nothing moves over several ticks' worth of time.
        const auto moves = [ambient] {
            const QVector<QPointF> before = ambient->spritePositions();
            QTest::qWait(150);
            return ambient->spritePositions() != before;
        };
        // Snow falls: every sprite moves down (one may wrap to the top).
        const QVector<QPointF> start = ambient->spritePositions();
        const auto fallen = [&] {
            const QVector<QPointF> now = ambient->spritePositions();
            int fell = 0;
            for (int i = 0; i < start.size(); ++i)
                fell += now[i].y() >= start[i].y() + 2 ? 1 : 0;
            return fell;
        };
        QTRY_VERIFY(fallen() >= start.size() - 1);

        stage.window.hide();
        QVERIFY(!ambient->animating());
        QVERIFY(!moves());
        stage.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&stage.window));
        QTRY_VERIFY(ambient->animating());

        stage.window.showMinimized();
        QTRY_COMPARE(stage.window.visibility(), QWindow::Minimized);
        QVERIFY(!ambient->animating());
        QVERIFY(!moves());
        stage.window.showNormal();
        QTRY_VERIFY(ambient->animating());

        ProfileRenderPolicy::instance().setLowMemoryMode(true);
        QVERIFY(!ambient->animating());
        QVERIFY(!moves());
        ProfileRenderPolicy::instance().setLowMemoryMode(false);
        QVERIFY(ambient->animating());

        ambient->setRunning(false);
        QVERIFY(!ambient->animating());
        QVERIFY(!moves());
        ambient->setRunning(true);
        ambient->setVisible(false);
        QVERIFY(!ambient->animating());
        ambient->setVisible(true);
        QVERIFY(ambient->animating());

        ambient->setKind(int(Profile::Ambient::NoAmbient));
        QCOMPARE(ambient->spriteCount(), 0);
        QVERIFY(!ambient->animating());

        // Sprites are the mockups' sizes and sit behind the boxes: small.
        ambient->setKind(int(Profile::Ambient::FallingHearts));
        const QList<QQuickItem *> sprites = ambient->childItems();
        QCOMPARE(sprites.size(), 8);
        for (QQuickItem *sprite : sprites)
            QVERIFY(sprite->width() <= 30 && sprite->height() <= 30);
        ambient->setSize(QSizeF(1920, 1080));
        QCOMPARE(ambient->spriteCount(), 28);
    }

    void tickerStopsWithoutActiveClients()
    {
        ProfileTicker &ticker = ProfileTicker::instance();
        QCOMPARE(ticker.subscriberCount(), 0);
        QVERIFY(!ticker.isRunning());

        ProfileTickerClient fast;
        fast.setFps(30);
        auto slow = std::make_unique<ProfileTickerClient>();
        slow->setFps(10);
        fast.setActive(true);
        slow->setActive(true);
        QVERIFY(ticker.isRunning());
        QTRY_VERIFY(fast.frame() >= 30);
        // The slow client gets a third of the clock's ticks.
        QVERIFY(std::abs(slow->frame() * 3 - fast.frame()) <= 3);

        fast.setActive(false);
        QVERIFY(ticker.isRunning()); // the slow one still wants it
        slow.reset();                // a destroyed client unsubscribes itself
        QCOMPARE(ticker.subscriberCount(), 0);
        QVERIFY(!ticker.isRunning());
        const int frame = ticker.frame();
        const int fastFrame = fast.frame();
        QTest::qWait(150);
        QCOMPARE(ticker.frame(), frame); // stopped means a stable frame
        QCOMPARE(fast.frame(), fastFrame);

        // Animation not allowed: the clock stays off even with clients.
        ProfileRenderPolicy::instance().setLowMemoryMode(true);
        fast.setActive(true);
        QVERIFY(!ticker.isRunning());
        QTest::qWait(120);
        QCOMPARE(fast.frame(), fastFrame);
        ProfileRenderPolicy::instance().setLowMemoryMode(false);
        QVERIFY(ticker.isRunning());
        QTRY_VERIFY(fast.frame() > fastFrame);
        fast.setActive(false);
        QVERIFY(!ticker.isRunning());
    }

    // ------------------------------------------------------------ small items

    void presetThumbPaintsTheOwnersName()
    {
        ProfileFonts::ensureRegistered();
        ProfilePresetThumb thumb;
        QCOMPARE(thumb.implicitWidth(), 129.0);
        QCOMPARE(thumb.implicitHeight(), 80.0);
        thumb.setSize(QSizeF(129, 80));
        thumb.setPreset(int(Profile::Preset::SceneQueenPreset));
        thumb.setOwnerName(QString());
        const QImage nameless = paintItem(thumb);
        thumb.setOwnerName(QStringLiteral("Daniel"));
        const QImage daniel = paintItem(thumb);
        thumb.setOwnerName(QStringLiteral("Zoe"));
        const QImage zoe = paintItem(thumb);
        QVERIFY(differingPixels(nameless, daniel) > 40);
        QVERIFY(differingPixels(daniel, zoe) > 20);
        // The name sits on the identity card's first line, and nowhere else.
        const QImage nameArea = daniel.copy(9, 8, 45, 14);
        QVERIFY(differingPixels(nameless.copy(9, 8, 45, 14), nameArea) > 40);
        QCOMPARE(differingPixels(nameless.copy(0, 40, 129, 40), daniel.copy(0, 40, 129, 40)), 0);

        // Only Aero Sky follows the viewer's mode.
        thumb.setDark(true);
        QCOMPARE(paintItem(thumb), zoe);
        thumb.setPreset(int(Profile::Preset::AeroSkyPreset));
        const QImage darkAero = paintItem(thumb);
        thumb.setDark(false);
        QVERIFY(differingPixels(paintItem(thumb), darkAero) > 2000);
        QVERIFY(ProfilePresetThumb::cachedCount() >= 5);

        if (!qEnvironmentVariableIsEmpty("OPENCHAT_PROFILE_CAPTURES"))
            captureThumbs();
    }

    void moodFacesDiffer()
    {
        // One mood for each expression.
        QList<int> moods;
        QList<int> faces;
        for (int mood = 1; mood <= Profile::maxMood; ++mood) {
            const int face = int(Profile::moodFace(Profile::Mood(mood)));
            if (!faces.contains(face)) {
                faces.push_back(face);
                moods.push_back(mood);
            }
        }
        QCOMPARE(faces.size(), 6);
        QList<QImage> images;
        QImage sheet(6 * 34 + 4, 38, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor(0xF4, 0xF8, 0xFC));
        QPainter sheetPainter(&sheet);
        for (int i = 0; i < moods.size(); ++i) {
            ProfileMoodFace face;
            QCOMPARE(face.implicitWidth(), 15.0);
            face.setSize(QSizeF(30, 30));
            face.setMood(moods[i]);
            QCOMPARE(face.face(), faces[i]);
            const QImage image = paintItem(face);
            QVERIFY(opaquePixels(image, 200) > 300);
            images.push_back(image);
            sheetPainter.drawImage(4 + i * 34, 4, image);
        }
        sheetPainter.end();
        capture(QStringLiteral("mood-faces"), sheet);
        for (int a = 0; a < images.size(); ++a) {
            for (int b = a + 1; b < images.size(); ++b)
                QVERIFY2(differingPixels(images[a], images[b], 24) > 6, qPrintable(QStringLiteral("%1 vs %2").arg(a).arg(b)));
        }
        ProfileMoodFace none;
        none.setSize(QSizeF(15, 15));
        QCOMPARE(opaquePixels(paintItem(none), 1), 0); // no mood, no face
    }

    void typesRegisterForQml()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine,
                                QUrl::fromLocalFile(QStringLiteral(OPENCHAT_SOURCE_DIR "/tests/qml/ProfileTypesSmoke.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        QCOMPARE(root->property("bubblesMotif").toInt(), int(Motif::Bubbles));
        QCOMPARE(root->property("glitterEffect").toInt(), int(Profile::NameEffect::GlitterName));
        QCOMPARE(root->property("customPreset").toInt(), int(Profile::Preset::CustomPreset));
        QCOMPARE(root->property("animationsAllowed").toBool(), true);
        QCOMPARE(root->property("lowMemoryMode").toBool(), false);
        ProfileRenderPolicy::instance().setLowMemoryMode(true);
        QCOMPARE(root->property("animationsAllowed").toBool(), false); // the singleton is the process instance

        auto *backdrop = root->findChild<ProfileBackdrop *>(QStringLiteral("profileBackdrop"));
        QVERIFY(backdrop);
        QCOMPARE(backdrop->motif(), int(Motif::Stars));
        QCOMPARE(backdrop->motifScale(), int(Profile::MotifScale::SmallMotif));
        QCOMPARE(backdrop->size(), QSizeF(420, 300));
        auto *layer = root->findChild<ProfileImageLayer *>(QStringLiteral("profileImageLayer"));
        QVERIFY(layer);
        QCOMPARE(layer->imageMode(), int(Profile::ImageMode::FitImage));
        auto *ambient = root->findChild<ProfileAmbient *>(QStringLiteral("profileAmbient"));
        QVERIFY(ambient);
        QCOMPARE(ambient->kind(), int(Profile::Ambient::FallingHearts));
        QCOMPARE(ambient->seed(), 7);
        auto *name = root->findChild<ProfileNameText *>(QStringLiteral("profileNameText"));
        QVERIFY(name);
        QCOMPARE(name->paintedText(), QStringLiteral("★ Michael ★"));
        QCOMPARE(name->x(), 12 - name->glyphLeft());
        auto *thumb = root->findChild<ProfilePresetThumb *>(QStringLiteral("profilePresetThumb"));
        QVERIFY(thumb);
        QCOMPARE(thumb->ownerName(), QStringLiteral("Daniel"));
        QCOMPARE(thumb->width(), 129.0);
        auto *mood = root->findChild<ProfileMoodFace *>(QStringLiteral("profileMoodFace"));
        QVERIFY(mood);
        QCOMPARE(mood->mood(), int(Profile::Mood::MoodHappy));
        auto *client = root->findChild<ProfileTickerClient *>(QStringLiteral("profileTickerClient"));
        QVERIFY(client);
        QCOMPARE(client->fps(), 10);
    }

private:
    QString requestAndWait(const QByteArray &jpeg)
    {
        ProfileMediaStore &store = ProfileMediaStore::instance();
        QSignalSpy ready(&store, &ProfileMediaStore::imageReady);
        const QString key = store.requestImage(jpeg);
        if (key.isEmpty() || store.image(key))
            return key;
        if (!ready.wait(10000))
            return {};
        return key;
    }

    static QString effectSlug(int effect)
    {
        static const char *names[] = {"none", "glow", "outline", "gradient", "glitter", "chrome", "shadow"};
        return QLatin1String(names[std::clamp(effect, 0, 6)]);
    }

    static QImage onBox(const QImage &frame, const QColor &box)
    {
        QImage out(frame.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(box);
        QPainter painter(&out);
        painter.drawImage(0, 0, frame);
        return out;
    }

    // Every motif at its preset's colours (or a neutral light palette) and a
    // contact sheet of all 17 at DPR 1 and 2.
    static void captureMotifs()
    {
        const QSize size(360, 240);
        QImage sheet(6 * 184 + 4, 3 * 144 + 4, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::white);
        QPainter sheetPainter(&sheet);
        for (int m = 0; m <= int(Motif::LinenWeave); ++m) {
            Motifs::BackdropSpec spec;
            spec.kind = Profile::BackgroundKind::PatternBackground;
            spec.motif = Motif(m);
            spec.color1 = QColor(0xF3, 0xEF, 0xE7);
            spec.color2 = QColor(0xEC, 0xE5, 0xD8);
            spec.ink = QColor(0x8A, 0x5A, 0xC0);
            spec.opacity = 0.5;
            for (int p = 0; p <= int(Profile::Preset::ChromeY2KPreset); ++p) {
                const Profile::Theme theme = Profile::presetTheme(Profile::Preset(p));
                if (theme.backgroundKind == Profile::BackgroundKind::PatternBackground && theme.motif == Motif(m)) {
                    spec = specOf(theme);
                    break;
                }
            }
            const QImage image = renderBackdrop(spec, size);
            capture(QStringLiteral("motif-%1").arg(motifSlug(Motif(m))), image);
            QImage hidpi(size * 2, QImage::Format_ARGB32_Premultiplied);
            hidpi.setDevicePixelRatio(2);
            hidpi.fill(Qt::transparent);
            {
                QPainter painter(&hidpi);
                Motifs::paintBackdrop(painter, QRectF(QPointF(0, 0), QSizeF(size)), spec);
            }
            capture(QStringLiteral("motif-%1@2x").arg(motifSlug(Motif(m))), hidpi);
            sheetPainter.drawImage(QRectF(4 + (m % 6) * 184, 4 + (m / 6) * 144, 180, 120), image);
            sheetPainter.setPen(Qt::black);
            sheetPainter.drawText(QPointF(8 + (m % 6) * 184, 138 + (m / 6) * 144), Profile::motifName(Motif(m)));
        }
        sheetPainter.end();
        capture(QStringLiteral("motifs-sheet"), sheet);

        // Each preset's whole backdrop at the default window, and the stub's aurora.
        for (int p = 0; p <= int(Profile::Preset::ChromeY2KPreset); ++p) {
            const Profile::Theme theme = Profile::presetTheme(Profile::Preset(p));
            capture(QStringLiteral("backdrop-%1").arg(presetSlugOf(p)), renderBackdrop(specOf(theme), QSize(860, 632)));
        }
        capture(QStringLiteral("backdrop-aero-sky-dark"),
                renderBackdrop(specOf(Profile::aeroSkyTheme(true)), QSize(860, 632)));
        Motifs::BackdropSpec aurora = specOf(Profile::aeroSkyTheme(false));
        aurora.aurora = true;
        capture(QStringLiteral("backdrop-stub-aurora"), renderBackdrop(aurora, QSize(860, 632)));
        aurora = specOf(Profile::aeroSkyTheme(true));
        aurora.aurora = true;
        capture(QStringLiteral("backdrop-stub-aurora-dark"), renderBackdrop(aurora, QSize(860, 632)));
    }

    // Each preset's name as its identity card shows it.
    static void captureNamePresets()
    {
        const QStringList owners{QStringLiteral("Michael"), QStringLiteral("Ryan"),    QStringLiteral("Jessica"),
                                 QStringLiteral("Jessica"), QStringLiteral("Alex"),    QStringLiteral("Sarah"),
                                 QStringLiteral("Sid"),     QStringLiteral("Daniel"),  QStringLiteral("Emma"),
                                 QStringLiteral("Ryan")};
        for (int p = 0; p <= int(Profile::Preset::ChromeY2KPreset); ++p) {
            for (const bool dark : {false, true}) {
                Profile::Theme theme = Profile::presetTheme(Profile::Preset(p));
                if (theme.adaptive)
                    theme = Profile::aeroSkyTheme(dark);
                else if (dark)
                    continue;
                const Readability::Palette palette = Readability::resolve(theme, Readability::pageSamples(theme, {}));
                ProfileNameText name;
                name.setText(owners[p]);
                name.setFlourish(int(theme.nameFlourish));
                name.setFontFamily(ProfileFonts::family(theme.nameFont, ProfileFonts::Role::Name));
                name.setBasePixelSize(ProfileFonts::namePixelSize(theme.nameFont, theme.nameSize));
                name.setColor(palette.name);
                name.setColor2(palette.name2);
                name.setEffect(int(theme.nameEffect));
                name.setDarkBox(palette.boxDark);
                name.setAvailableWidth(297);
                name.setAnimate(false);
                name.setSize(QSizeF(name.implicitWidth(), name.implicitHeight()));
                QImage frame(name.size().toSize() * 2, QImage::Format_ARGB32_Premultiplied);
                frame.setDevicePixelRatio(2);
                frame.fill(palette.boxBackgrounds.first());
                QPainter painter(&frame);
                painter.drawImage(QPointF(0, 0), name.renderFrame(2.0));
                painter.end();
                capture(QStringLiteral("name-preset-%1%2").arg(presetSlugOf(p), dark ? QStringLiteral("-dark") : QString()),
                        frame);
                if (theme.nameEffect == Profile::NameEffect::GlitterName) {
                    name.setPhase(5);
                    capture(QStringLiteral("name-preset-%1-frame5").arg(presetSlugOf(p)), onBox(name.renderFrame(1.0),
                                                                                                 palette.boxBackgrounds.first()));
                }
            }
        }
    }

    static void captureThumbs()
    {
        QImage sheet(QSize(2 * 141 + 12, 6 * 96 + 12) * 2, QImage::Format_ARGB32_Premultiplied);
        sheet.setDevicePixelRatio(2);
        sheet.fill(QColor(0xF4, 0xF8, 0xFC));
        QPainter painter(&sheet);
        int index = 0;
        for (int p = 0; p <= int(Profile::Preset::ChromeY2KPreset) + 1; ++p) {
            ProfilePresetThumb thumb;
            thumb.setSize(QSizeF(129, 80));
            thumb.setPreset(std::min(p, int(Profile::Preset::ChromeY2KPreset)));
            thumb.setDark(p > int(Profile::Preset::ChromeY2KPreset)); // the last one: Aero Sky dark
            if (p > int(Profile::Preset::ChromeY2KPreset))
                thumb.setPreset(int(Profile::Preset::AeroSkyPreset));
            thumb.setOwnerName(QStringLiteral("Daniel"));
            const QImage image = thumb.render(2.0);
            capture(QStringLiteral("thumb-%1%2").arg(presetSlugOf(thumb.preset()),
                                                     thumb.dark() ? QStringLiteral("-dark") : QString()),
                    image);
            painter.drawImage(QPointF(6 + (index % 2) * 141, 6 + (index / 2) * 96), image);
            ++index;
        }
        painter.end();
        capture(QStringLiteral("thumbs-sheet"), sheet);
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    ProfileRenderTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_profilerender.moc"
