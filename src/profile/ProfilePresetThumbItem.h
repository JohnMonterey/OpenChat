#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QString>

namespace OpenChat {

// A live miniature of one preset for the editor's Themes tab (SPEC §14.5; the
// mockups' ThemeThumb): the preset's own backdrop at 0.55 scale, the
// two-column box skeleton in its box, strip and border colours, and the
// owner's name in its name face and effect (a still frame). Only Aero Sky
// follows `dark`; every other preset looks the same in both modes. No timers,
// no ambient. Frames are cached per (preset, name, mode, size, device scale).
class ProfilePresetThumb : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int preset READ preset WRITE setPreset NOTIFY presetChanged) // Profile.Preset
    Q_PROPERTY(QString ownerName READ ownerName WRITE setOwnerName NOTIFY ownerNameChanged)
    Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY darkChanged)

public:
    static constexpr qreal defaultWidth = 129;
    static constexpr qreal defaultHeight = 80;

    explicit ProfilePresetThumb(QQuickItem *parent = nullptr);

    [[nodiscard]] int preset() const noexcept { return m_preset; }
    void setPreset(int preset);
    [[nodiscard]] QString ownerName() const { return m_ownerName; }
    void setOwnerName(const QString &name);
    [[nodiscard]] bool dark() const noexcept { return m_dark; }
    void setDark(bool dark);

    // The miniature at `dpr`, as the item paints it.
    [[nodiscard]] QImage render(qreal dpr) const;
    [[nodiscard]] static int cachedCount();

    void paint(QPainter *painter) override;

signals:
    void presetChanged();
    void ownerNameChanged();
    void darkChanged();

private:
    void paintThumb(QPainter &painter) const;

    int m_preset = 0;
    QString m_ownerName;
    bool m_dark = false;
};

} // namespace OpenChat
