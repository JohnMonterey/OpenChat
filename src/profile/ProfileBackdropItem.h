#pragma once

#include "profile/ProfileMotifs.h"

#include <QColor>
#include <QQuickPaintedItem>

namespace OpenChat {

// The page's backdrop (SPEC §3.2, §8): the base colour or vertical gradient
// and the pattern, fixed behind the scrolling columns. It is the page's only
// viewport-sized raster, repainted only when its size, device pixel ratio or
// a property changes; a pattern costs one cached tile repeated over it. A
// picture background draws only its base here: the photo is
// ProfileImageLayer, a scene-graph texture above it.
class ProfileBackdrop : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int kind READ kind WRITE setKind NOTIFY kindChanged) // Profile.BackgroundKind
    Q_PROPERTY(QColor color1 READ color1 WRITE setColor1 NOTIFY color1Changed)
    Q_PROPERTY(QColor color2 READ color2 WRITE setColor2 NOTIFY color2Changed)
    Q_PROPERTY(int motif READ motif WRITE setMotif NOTIFY motifChanged)
    Q_PROPERTY(int motifScale READ motifScale WRITE setMotifScale NOTIFY motifScaleChanged)
    Q_PROPERTY(QColor motifInk READ motifInk WRITE setMotifInk NOTIFY motifInkChanged)
    Q_PROPERTY(qreal motifOpacity READ motifOpacity WRITE setMotifOpacity NOTIFY motifOpacityChanged) // 0…1
    // The stub page's calm backdrop: the base and the bubbles' ribbons only.
    Q_PROPERTY(bool aurora READ aurora WRITE setAurora NOTIFY auroraChanged)
    // Selects the ribbons' dark-base alphas (a base darker than luminance
    // 0.18 does so on its own).
    Q_PROPERTY(bool darkBase READ darkBase WRITE setDarkBase NOTIFY darkBaseChanged)
    // Miniatures draw the whole backdrop at this scale so a motif reads as a
    // pattern in a 46×40 swatch (the mockups use 0.5). 1 on the page.
    Q_PROPERTY(qreal previewScale READ previewScale WRITE setPreviewScale NOTIFY previewScaleChanged)

public:
    explicit ProfileBackdrop(QQuickItem *parent = nullptr);

    [[nodiscard]] int kind() const noexcept { return int(m_spec.kind); }
    void setKind(int kind);
    [[nodiscard]] QColor color1() const { return m_spec.color1; }
    void setColor1(const QColor &color);
    [[nodiscard]] QColor color2() const { return m_spec.color2; }
    void setColor2(const QColor &color);
    [[nodiscard]] int motif() const noexcept { return int(m_spec.motif); }
    void setMotif(int motif);
    [[nodiscard]] int motifScale() const noexcept { return int(m_spec.scale); }
    void setMotifScale(int scale);
    [[nodiscard]] QColor motifInk() const { return m_spec.ink; }
    void setMotifInk(const QColor &color);
    [[nodiscard]] qreal motifOpacity() const noexcept { return m_spec.opacity; }
    void setMotifOpacity(qreal opacity);
    [[nodiscard]] bool aurora() const noexcept { return m_spec.aurora; }
    void setAurora(bool aurora);
    [[nodiscard]] bool darkBase() const noexcept { return m_spec.darkBase; }
    void setDarkBase(bool dark);
    [[nodiscard]] qreal previewScale() const noexcept { return m_previewScale; }
    void setPreviewScale(qreal scale);

    [[nodiscard]] const ProfileMotifs::BackdropSpec &spec() const noexcept { return m_spec; }
    // Times paint() ran (tests: nothing repaints the backdrop while scrolling).
    [[nodiscard]] int paintCount() const noexcept { return m_paintCount; }

    void paint(QPainter *painter) override;

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

signals:
    void kindChanged();
    void color1Changed();
    void color2Changed();
    void motifChanged();
    void motifScaleChanged();
    void motifInkChanged();
    void motifOpacityChanged();
    void auroraChanged();
    void darkBaseChanged();
    void previewScaleChanged();

private:
    ProfileMotifs::BackdropSpec m_spec;
    qreal m_previewScale = 1.0;
    int m_paintCount = 0;
};

} // namespace OpenChat
