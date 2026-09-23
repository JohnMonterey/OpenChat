#pragma once

#include <QImage>
#include <QMarginsF>
#include <QQuickPaintedItem>
#include <QTimer>

namespace OpenChat {

// Draws an equipped avatar frame around a display picture. The item is larger
// than the picture by its insets (see insetsFor); the picture sits at
// (insetLeft, insetTop). Frames are rendered once per (id, size, theme, dpr,
// phase) into a shared cache, so a frame repeated down a list or ticking
// through its animation phases costs one image blit per paint.
class AvatarFrame : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString frameId READ frameId WRITE setFrameId NOTIFY frameIdChanged)
    Q_PROPERTY(qreal avatarSize READ avatarSize WRITE setAvatarSize NOTIFY avatarSizeChanged)
    Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    // Animated frames advance on their own while visible; the gallery and the
    // tests switch it off for a stable picture.
    Q_PROPERTY(bool animate READ animate WRITE setAnimate NOTIFY animateChanged)
    Q_PROPERTY(int phase READ phase WRITE setPhase NOTIFY phaseChanged)
    Q_PROPERTY(qreal insetLeft READ insetLeft NOTIFY insetsChanged)
    Q_PROPERTY(qreal insetTop READ insetTop NOTIFY insetsChanged)
    Q_PROPERTY(qreal insetRight READ insetRight NOTIFY insetsChanged)
    Q_PROPERTY(qreal insetBottom READ insetBottom NOTIFY insetsChanged)

  public:
    explicit AvatarFrame(QQuickItem *parent = nullptr);

    [[nodiscard]] QString frameId() const { return m_frameId; }
    void setFrameId(const QString &frameId);
    [[nodiscard]] qreal avatarSize() const { return m_avatarSize; }
    void setAvatarSize(qreal size);
    [[nodiscard]] qreal cornerRadius() const { return m_cornerRadius; }
    void setCornerRadius(qreal radius);
    [[nodiscard]] bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    [[nodiscard]] bool animate() const { return m_animate; }
    void setAnimate(bool animate);
    [[nodiscard]] int phase() const { return m_phase; }
    void setPhase(int phase);

    [[nodiscard]] qreal insetLeft() const { return insets().left(); }
    [[nodiscard]] qreal insetTop() const { return insets().top(); }
    [[nodiscard]] qreal insetRight() const { return insets().right(); }
    [[nodiscard]] qreal insetBottom() const { return insets().bottom(); }

    // How far a frame reaches beyond a picture of `avatarSize` on each side.
    static QMarginsF insetsFor(const QString &frameId, qreal avatarSize);
    // Animation phases a frame cycles through; 1 for a still frame.
    static int phaseCountFor(const QString &frameId);
    // The frame alone, rendered (or fetched from the cache) at `dpr`, sized to
    // the picture plus its insets.
    static QImage render(const QString &frameId, qreal avatarSize, qreal cornerRadius, bool dark,
                         int phase, qreal dpr);

    void paint(QPainter *painter) override;

  signals:
    void frameIdChanged();
    void avatarSizeChanged();
    void cornerRadiusChanged();
    void darkModeChanged();
    void animateChanged();
    void phaseChanged();
    void insetsChanged();

  protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;

  private:
    [[nodiscard]] QMarginsF insets() const { return insetsFor(m_frameId, m_avatarSize); }
    void updateTicker();

    QString m_frameId;
    qreal m_avatarSize = 44.0;
    qreal m_cornerRadius = 5.0;
    bool m_darkMode = false;
    bool m_animate = true;
    int m_phase = 0;
    QTimer m_ticker;
};

} // namespace OpenChat
