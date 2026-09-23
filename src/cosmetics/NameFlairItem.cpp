#include "cosmetics/NameFlairItem.h"

#include "cosmetics/CosmeticPaint.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <array>
#include <cmath>

namespace OpenChat {

using namespace CosmeticPaint;

namespace {

qreal pixelSizeOf(const QFont &font)
{
    if (font.pixelSize() > 0)
        return font.pixelSize();
    return font.pointSizeF() > 0 ? font.pointSizeF() * 96.0 / 72.0 : 14.0;
}

// Everything a flair needs to know about the words it is inking.
struct Words
{
    QString text;
    QFont font;
    QPainterPath path;  // the glyph outlines, positioned on the baseline
    QPointF origin;     // start of the baseline
    QRectF box;         // advance box: ascent to descent
    qreal capTop = 0;   // top of the capitals
    qreal baseline = 0;
    qreal descent = 0;
    qreal size = 14;    // pixel size
    bool dark = false;
    QColor textColor;
};

// Where the top of a glyph sits, for placing glints on letters.
QPointF letterTop(const Words &w, int index, qreal along = 0.5)
{
    const QFontMetricsF fm(w.font);
    const qreal left = fm.horizontalAdvance(w.text.left(index));
    const qreal advance = fm.horizontalAdvance(w.text.mid(index, 1));
    return {w.origin.x() + left + advance * along, w.capTop};
}

// Picks the index of a letter (not a space) near `fraction` of the way along.
int letterNear(const Words &w, qreal fraction)
{
    if (w.text.isEmpty())
        return 0;
    int index = std::clamp(int(std::round(fraction * (w.text.size() - 1))), 0, int(w.text.size()) - 1);
    for (int step = 0; step < w.text.size(); ++step) {
        const int forward = std::min(int(w.text.size()) - 1, index + step);
        if (w.text.at(forward).isLetterOrNumber())
            return forward;
        const int back = std::max(0, index - step);
        if (w.text.at(back).isLetterOrNumber())
            return back;
    }
    return index;
}

void drawPathGlow(QPainter &p, const Words &w, const QColor &colour, qreal spread, qreal blur)
{
    drawBlurred(p, w.box, blur, [&](QPainter &lp) {
        lp.setPen(QPen(colour, spread, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        lp.setBrush(colour);
        lp.drawPath(w.path);
    });
}

void strokeOutline(QPainter &p, const Words &w, const QColor &colour, qreal width)
{
    p.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(w.path);
}

QLinearGradient verticalInk(const Words &w)
{
    return QLinearGradient(0, w.capTop, 0, w.baseline + w.descent * 0.6);
}

// ------------------------------------------------------------------ Aero Glow

void paintAeroGlow(QPainter &p, const Words &w)
{
    // Vista put a soft halo behind caption text so it read on any glass.
    drawPathGlow(p, w, w.dark ? QColor(70, 160, 255, 210) : QColor(120, 196, 255, 230),
                 w.size * 0.22, w.size * 0.3);
    drawPathGlow(p, w, w.dark ? QColor(140, 205, 255, 120) : QColor(255, 255, 255, 250),
                 w.size * 0.12, w.size * 0.1);
    p.setPen(Qt::NoPen);
    p.setBrush(w.textColor);
    p.drawPath(w.path);
}

// ----------------------------------------------------------------- Y2K Chrome

void paintChrome(QPainter &p, const Words &w)
{
    if (w.dark)
        strokeOutline(p, w, QColor(150, 200, 245, 90), w.size * 0.17);
    strokeOutline(p, w, w.dark ? QColor(6, 10, 18) : QColor(28, 40, 58), w.size * 0.085);
    // Sky above a hard horizon, ground below: the chrome of every Y2K logo.
    QLinearGradient chrome = verticalInk(w);
    chrome.setColorAt(0.00, QColor(214, 236, 255));
    chrome.setColorAt(0.30, QColor(255, 255, 255));
    chrome.setColorAt(0.49, QColor(176, 192, 212));
    chrome.setColorAt(0.52, QColor(70, 82, 100));
    chrome.setColorAt(0.60, QColor(124, 138, 156));
    chrome.setColorAt(0.84, QColor(222, 230, 240));
    chrome.setColorAt(1.00, QColor(255, 255, 255));
    p.setPen(Qt::NoPen);
    p.setBrush(chrome);
    p.drawPath(w.path);
    drawGlint(p, letterTop(w, letterNear(w, 0.0), 0.72) + QPointF(0, w.size * 0.05),
              w.size * 0.3, QColor(200, 230, 255));
}

// ------------------------------------------------------------------ Gold Leaf

void paintGold(QPainter &p, const Words &w)
{
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(50, 24, 0, w.dark ? 200 : 110));
    p.drawPath(w.path.translated(0.5, w.size * 0.07));
    strokeOutline(p, w, w.dark ? QColor(40, 22, 0) : QColor(96, 60, 8), w.size * 0.1);
    // A bright rim along the top of every stroke, as leaf catches light.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 246, 204));
    p.drawPath(w.path.translated(0, -w.size * 0.035));
    QLinearGradient gold = verticalInk(w);
    gold.setColorAt(0.00, QColor(255, 244, 196));
    gold.setColorAt(0.28, QColor(246, 206, 96));
    gold.setColorAt(0.52, QColor(184, 126, 28));
    gold.setColorAt(0.62, QColor(214, 158, 56));
    gold.setColorAt(0.85, QColor(248, 216, 128));
    gold.setColorAt(1.00, QColor(160, 104, 22));
    p.setBrush(gold);
    p.drawPath(w.path);
    drawGlint(p, letterTop(w, letterNear(w, 0.8), 0.6), w.size * 0.26, QColor(255, 236, 170));
}

// ------------------------------------------------------------------ Holo Foil

void paintHolo(QPainter &p, const Words &w)
{
    strokeOutline(p, w, w.dark ? QColor(26, 16, 52) : QColor(44, 30, 92), w.size * 0.11);
    const qreal run = std::max(40.0, w.size * 3.0);
    QLinearGradient foil(w.box.topLeft(), w.box.topLeft() + QPointF(run, run * 0.35));
    foil.setSpread(QGradient::RepeatSpread);
    const std::array<QColor, 6> spectrum =
        w.dark ? std::array<QColor, 6>{QColor(255, 146, 214), QColor(255, 214, 128),
                                       QColor(164, 255, 184), QColor(128, 228, 255),
                                       QColor(186, 158, 255), QColor(255, 146, 214)}
               : std::array<QColor, 6>{QColor(226, 64, 160), QColor(234, 150, 26),
                                       QColor(30, 176, 104), QColor(22, 150, 216),
                                       QColor(122, 78, 224), QColor(226, 64, 160)};
    for (size_t i = 0; i < spectrum.size(); ++i)
        foil.setColorAt(qreal(i) / (spectrum.size() - 1), spectrum[i]);
    p.setPen(Qt::NoPen);
    p.setBrush(foil);
    p.drawPath(w.path);
    // A diagonal sheen across the foil.
    p.save();
    p.setClipPath(w.path);
    const qreal x = w.box.left() + w.box.width() * 0.34;
    QLinearGradient sheen(QPointF(x, 0), QPointF(x + w.size * 1.2, 0));
    sheen.setColorAt(0.0, QColor(255, 255, 255, 0));
    sheen.setColorAt(0.5, QColor(255, 255, 255, w.dark ? 170 : 150));
    sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
    QTransform skew;
    skew.translate(0, w.box.center().y());
    skew.shear(-0.5, 0);
    skew.translate(0, -w.box.center().y());
    p.setTransform(skew, true);
    p.fillRect(QRectF(x - w.size, w.box.top() - 4, w.size * 3.2, w.box.height() + 8), sheen);
    p.restore();
    drawGlint(p, letterTop(w, letterNear(w, 0.12), 0.3), w.size * 0.28, QColor(255, 220, 250));
    drawGlint(p, letterTop(w, letterNear(w, 1.0), 0.85) + QPointF(0, w.size * 0.04),
              w.size * 0.2, QColor(210, 250, 255), 0.9);
}

// ------------------------------------------------------------------ Neon Sign

void paintNeon(QPainter &p, const Words &w)
{
    const QColor tube = w.dark ? QColor(255, 84, 214) : QColor(226, 22, 158);
    drawPathGlow(p, w, withAlpha(tube, w.dark ? 0.9 : 0.34), w.size * 0.2, w.size * 0.34);
    drawPathGlow(p, w, withAlpha(tube, w.dark ? 0.95 : 0.4), w.size * 0.08, w.size * 0.1);
    if (w.dark) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 238, 252));
        p.drawPath(w.path);
        strokeOutline(p, w, withAlpha(tube, 0.95), w.size * 0.045);
    } else {
        // By day the tube is the colour and the outline gives it an edge.
        strokeOutline(p, w, QColor(120, 0, 80, 200), w.size * 0.07);
        QLinearGradient ink = verticalInk(w);
        ink.setColorAt(0.0, QColor(255, 96, 208));
        ink.setColorAt(1.0, QColor(200, 12, 138));
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPath(w.path);
        // The hot core of the tube, just inside each stroke.
        p.save();
        p.setClipPath(w.path);
        p.setPen(QPen(QColor(255, 220, 246, 210), w.size * 0.045));
        p.setBrush(Qt::NoBrush);
        p.drawPath(w.path.translated(0, w.size * 0.05));
        p.restore();
    }
}

// --------------------------------------------------------------------- Molten

void paintEmber(QPainter &p, const Words &w)
{
    drawPathGlow(p, w, QColor(255, 96, 0, w.dark ? 210 : 130), w.size * 0.16, w.size * 0.3);
    strokeOutline(p, w, w.dark ? QColor(70, 10, 0, 220) : QColor(90, 14, 0), w.size * 0.1);
    QLinearGradient molten = verticalInk(w);
    molten.setColorAt(0.00, QColor(255, 252, 214));
    molten.setColorAt(0.30, QColor(255, 214, 58));
    molten.setColorAt(0.62, QColor(255, 116, 0));
    molten.setColorAt(1.00, QColor(200, 22, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(molten);
    p.drawPath(w.path);
    // Sparks lifting off the letters: tiny white-hot specks.
    const quint32 seed = 0xE3B3u;
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 6; ++i) {
        const qreal x = w.box.left() + w.box.width() * (0.06 + 0.88 * random01(seed, i));
        const qreal y = w.capTop - w.size * (0.04 + 0.32 * random01(seed + 1, i));
        const qreal r = w.size * (0.022 + 0.03 * random01(seed + 2, i));
        QRadialGradient spark(QPointF(x, y), r * 2.2);
        spark.setColorAt(0.0, QColor(255, 255, 236));
        spark.setColorAt(0.3, QColor(255, 206, 90, 240));
        spark.setColorAt(0.6, QColor(255, 120, 10, 120));
        spark.setColorAt(1.0, QColor(255, 90, 0, 0));
        p.setBrush(spark);
        p.drawEllipse(QPointF(x, y), r * 2.2, r * 2.2);
    }
}

} // namespace

NameFlair::NameFlair(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

void NameFlair::setFlairId(const QString &flairId)
{
    if (m_flairId == flairId)
        return;
    m_flairId = flairId;
    emit flairIdChanged();
    update();
}

void NameFlair::setText(const QString &text)
{
    if (m_text == text)
        return;
    m_text = text;
    emit textChanged();
    update();
}

void NameFlair::setFont(const QFont &font)
{
    if (m_font == font)
        return;
    m_font = font;
    emit fontChanged();
    emit paddingChanged();
    update();
}

void NameFlair::setTextColor(const QColor &color)
{
    if (m_textColor == color)
        return;
    m_textColor = color;
    emit textColorChanged();
    update();
}

void NameFlair::setDarkMode(bool dark)
{
    if (m_darkMode == dark)
        return;
    m_darkMode = dark;
    emit darkModeChanged();
    update();
}

void NameFlair::setTextWidth(qreal width)
{
    if (qFuzzyCompare(m_textWidth, width))
        return;
    m_textWidth = width;
    emit textWidthChanged();
    update();
}

void NameFlair::setElided(bool elided)
{
    if (m_elided == elided)
        return;
    m_elided = elided;
    emit elidedChanged();
    update();
}

qreal NameFlair::padding() const
{
    return std::ceil(std::max(4.0, pixelSizeOf(m_font) * 0.45));
}

void NameFlair::paint(QPainter *painter)
{
    if (m_flairId.isEmpty() || m_text.isEmpty())
        return;
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);

    const QFontMetricsF metrics(m_font);
    const qreal pad = padding();
    Words w;
    w.font = m_font;
    w.text = m_elided ? metrics.elidedText(m_text, Qt::ElideRight, m_textWidth) : m_text;
    w.size = pixelSizeOf(m_font);
    w.baseline = pad + metrics.ascent();
    w.descent = metrics.descent();
    w.capTop = w.baseline - metrics.capHeight();
    w.origin = QPointF(pad, w.baseline);
    w.box = QRectF(pad, pad, metrics.horizontalAdvance(w.text), metrics.ascent() + metrics.descent());
    w.dark = m_darkMode;
    w.textColor = m_textColor;
    w.path.addText(w.origin, m_font, w.text);

    if (m_flairId == QLatin1String("flair.aero"))
        paintAeroGlow(*painter, w);
    else if (m_flairId == QLatin1String("flair.chrome"))
        paintChrome(*painter, w);
    else if (m_flairId == QLatin1String("flair.gold"))
        paintGold(*painter, w);
    else if (m_flairId == QLatin1String("flair.holo"))
        paintHolo(*painter, w);
    else if (m_flairId == QLatin1String("flair.neon"))
        paintNeon(*painter, w);
    else if (m_flairId == QLatin1String("flair.ember"))
        paintEmber(*painter, w);
}

} // namespace OpenChat
