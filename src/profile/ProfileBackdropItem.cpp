#include "profile/ProfileBackdropItem.h"

#include <QPainter>

#include <algorithm>

namespace OpenChat {

namespace {

// QML hands enums over as ints; anything out of range is ignored.
bool validKind(int kind)
{
    return kind >= 0 && kind <= int(Profile::BackgroundKind::ImageBackground);
}

bool validMotif(int motif)
{
    return motif >= 0 && motif <= int(Profile::Motif::LinenWeave);
}

bool validScale(int scale)
{
    return scale >= 0 && scale <= int(Profile::MotifScale::LargeMotif);
}

} // namespace

ProfileBackdrop::ProfileBackdrop(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    // Opaque: the base covers every pixel, so the scene graph need not blend
    // it with whatever lies behind the page.
    setOpaquePainting(true);
    setAntialiasing(true);
}

void ProfileBackdrop::setKind(int kind)
{
    if (!validKind(kind) || kind == int(m_spec.kind))
        return;
    m_spec.kind = Profile::BackgroundKind(kind);
    emit kindChanged();
    update();
}

void ProfileBackdrop::setColor1(const QColor &color)
{
    if (color == m_spec.color1)
        return;
    m_spec.color1 = color;
    emit color1Changed();
    update();
}

void ProfileBackdrop::setColor2(const QColor &color)
{
    if (color == m_spec.color2)
        return;
    m_spec.color2 = color;
    emit color2Changed();
    update();
}

void ProfileBackdrop::setMotif(int motif)
{
    if (!validMotif(motif) || motif == int(m_spec.motif))
        return;
    m_spec.motif = Profile::Motif(motif);
    emit motifChanged();
    update();
}

void ProfileBackdrop::setMotifScale(int scale)
{
    if (!validScale(scale) || scale == int(m_spec.scale))
        return;
    m_spec.scale = Profile::MotifScale(scale);
    emit motifScaleChanged();
    update();
}

void ProfileBackdrop::setMotifInk(const QColor &color)
{
    if (color == m_spec.ink)
        return;
    m_spec.ink = color;
    emit motifInkChanged();
    update();
}

void ProfileBackdrop::setMotifOpacity(qreal opacity)
{
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (qFuzzyCompare(opacity + 1.0, m_spec.opacity + 1.0))
        return;
    m_spec.opacity = opacity;
    emit motifOpacityChanged();
    update();
}

void ProfileBackdrop::setAurora(bool aurora)
{
    if (aurora == m_spec.aurora)
        return;
    m_spec.aurora = aurora;
    emit auroraChanged();
    update();
}

void ProfileBackdrop::setDarkBase(bool dark)
{
    if (dark == m_spec.darkBase)
        return;
    m_spec.darkBase = dark;
    emit darkBaseChanged();
    update();
}

void ProfileBackdrop::setPreviewScale(qreal scale)
{
    scale = std::clamp(scale, 0.1, 4.0);
    if (qFuzzyCompare(scale, m_previewScale))
        return;
    m_previewScale = scale;
    emit previewScaleChanged();
    update();
}

void ProfileBackdrop::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update();
}

void ProfileBackdrop::paint(QPainter *painter)
{
    ++m_paintCount;
    ProfileMotifs::paintBackdrop(*painter, QRectF(0, 0, width(), height()), m_spec, m_previewScale);
}

} // namespace OpenChat
