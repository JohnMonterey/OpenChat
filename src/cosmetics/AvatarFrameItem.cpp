#include "cosmetics/AvatarFrameItem.h"

#include "cosmetics/CosmeticPaint.h"
#include "cosmetics/FramePainters.h"

#include <QCache>
#include <QPainter>
#include <QQuickWindow>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace OpenChat {

using namespace CosmeticPaint;

namespace {

QCache<QString, QImage> &frameCache()
{
    // Costs are in kilobytes: a few megabytes covers every frame at every size
    // the interface shows, in both themes, with all animation phases.
    static QCache<QString, QImage> cache(24 * 1024);
    return cache;
}

} // namespace

AvatarFrame::AvatarFrame(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    m_ticker.setTimerType(Qt::CoarseTimer);
    connect(&m_ticker, &QTimer::timeout, this, [this] {
        const int count = phaseCountFor(m_frameId);
        if (count > 1)
            setPhase((m_phase + 1) % count);
    });
}

void AvatarFrame::setFrameId(const QString &frameId)
{
    if (m_frameId == frameId)
        return;
    m_frameId = frameId;
    m_phase = 0;
    emit frameIdChanged();
    emit insetsChanged();
    updateTicker();
    update();
}

void AvatarFrame::setAvatarSize(qreal size)
{
    size = std::max(8.0, size);
    if (qFuzzyCompare(m_avatarSize, size))
        return;
    m_avatarSize = size;
    emit avatarSizeChanged();
    emit insetsChanged();
    update();
}

void AvatarFrame::setCornerRadius(qreal radius)
{
    radius = std::max(0.0, radius);
    if (qFuzzyCompare(m_cornerRadius, radius))
        return;
    m_cornerRadius = radius;
    emit cornerRadiusChanged();
    update();
}

void AvatarFrame::setDarkMode(bool dark)
{
    if (m_darkMode == dark)
        return;
    m_darkMode = dark;
    emit darkModeChanged();
    update();
}

void AvatarFrame::setAnimate(bool animate)
{
    if (m_animate == animate)
        return;
    m_animate = animate;
    emit animateChanged();
    updateTicker();
}

void AvatarFrame::setPhase(int phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    emit phaseChanged();
    update();
}

void AvatarFrame::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged || change == ItemSceneChange)
        updateTicker();
}

void AvatarFrame::updateTicker()
{
    const int count = phaseCountFor(m_frameId);
    const bool run = m_animate && count > 1 && isVisible() && window();
    if (run && !m_ticker.isActive())
        m_ticker.start(FramePainters::phaseInterval(m_frameId));
    else if (!run)
        m_ticker.stop();
}

QMarginsF AvatarFrame::insetsFor(const QString &frameId, qreal avatarSize)
{
    if (frameId.isEmpty())
        return {};
    const qreal side = FramePainters::sideMargin(avatarSize);
    return {side, side + FramePainters::extraTop(frameId, avatarSize), side, side};
}

int AvatarFrame::phaseCountFor(const QString &frameId)
{
    return FramePainters::phaseCount(frameId);
}

QImage AvatarFrame::render(const QString &frameId, qreal avatarSize, qreal cornerRadius, bool dark,
                           int phase, qreal dpr)
{
    if (frameId.isEmpty())
        return {};
    const int phases = phaseCountFor(frameId);
    phase = phases > 1 ? ((phase % phases) + phases) % phases : 0;
    const QString key = QStringLiteral("%1|%2|%3|%4|%5|%6")
                            .arg(frameId)
                            .arg(avatarSize, 0, 'f', 2)
                            .arg(cornerRadius, 0, 'f', 2)
                            .arg(dark ? 1 : 0)
                            .arg(phase)
                            .arg(dpr, 0, 'f', 3);
    if (const QImage *cached = frameCache().object(key))
        return *cached;

    const QMarginsF in = insetsFor(frameId, avatarSize);
    const QSizeF size(avatarSize + in.left() + in.right(), avatarSize + in.top() + in.bottom());
    QImage image = transparentImage(size, dpr);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        FramePainters::Canvas canvas;
        canvas.bounds = QRectF(QPointF(0, 0), size);
        canvas.picture = QRectF(in.left(), in.top(), avatarSize, avatarSize);
        canvas.radius = std::clamp(cornerRadius, 0.0, avatarSize / 2.0);
        canvas.margin = in.left();
        canvas.scale = avatarSize / 44.0;
        canvas.dark = dark;
        canvas.phase = phase;
        canvas.phases = phases;
        canvas.dpr = dpr;
        FramePainters::paint(painter, frameId, canvas);
    }
    const int cost = std::max<qsizetype>(1, image.sizeInBytes() / 1024);
    frameCache().insert(key, new QImage(image), cost);
    return image;
}

void AvatarFrame::paint(QPainter *painter)
{
    if (m_frameId.isEmpty())
        return;
    const qreal dpr = deviceScale(*painter);
    const QImage image = render(m_frameId, m_avatarSize, m_cornerRadius, m_darkMode, m_phase, dpr);
    if (image.isNull())
        return;
    painter->drawImage(QPointF(0, 0), image);
}

} // namespace OpenChat
