#pragma once

#include <QImage>
#include <QQuickPaintedItem>

namespace OpenChat {

// An alternative presence bead: the state colour (green Available, amber Away,
// grey Offline, red Busy) stays the dominant ink while the style changes the
// shape and material. The bead proper is `beadSize` across, centred; the item
// is `overhang` larger on every side for glows and rings.
class BeadArt : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString styleId READ styleId WRITE setStyleId NOTIFY styleIdChanged)
    Q_PROPERTY(int presence READ presence WRITE setPresence NOTIFY presenceChanged)
    Q_PROPERTY(qreal beadSize READ beadSize WRITE setBeadSize NOTIFY beadSizeChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal overhang READ overhang NOTIFY overhangChanged)

  public:
    explicit BeadArt(QQuickItem *parent = nullptr);

    [[nodiscard]] QString styleId() const { return m_styleId; }
    void setStyleId(const QString &styleId);
    [[nodiscard]] int presence() const { return m_presence; }
    void setPresence(int presence);
    [[nodiscard]] qreal beadSize() const { return m_beadSize; }
    void setBeadSize(qreal size);
    [[nodiscard]] bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    [[nodiscard]] qreal overhang() const { return overhangFor(m_styleId, m_beadSize); }

    static qreal overhangFor(const QString &styleId, qreal beadSize);
    static QImage render(const QString &styleId, int presence, qreal beadSize, bool dark, qreal dpr);

    void paint(QPainter *painter) override;

  signals:
    void styleIdChanged();
    void presenceChanged();
    void beadSizeChanged();
    void darkModeChanged();
    void overhangChanged();

  private:
    QString m_styleId;
    int m_presence = 0;
    qreal m_beadSize = 12.0;
    bool m_darkMode = false;
};

} // namespace OpenChat
