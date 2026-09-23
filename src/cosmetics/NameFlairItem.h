#pragma once

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>

namespace OpenChat {

// Draws a display name with an equipped flair. It is laid over the plain Text
// it replaces, which stays in place (transparent) to keep the layout: the name
// keeps its font, size, width and elision, so the bead after it never moves.
// The item reaches `padding` past the text on every side for glows.
class NameFlair : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString flairId READ flairId WRITE setFlairId NOTIFY flairIdChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor NOTIFY textColorChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    // The width the plain Text was given, and whether it had to elide in it.
    Q_PROPERTY(qreal textWidth READ textWidth WRITE setTextWidth NOTIFY textWidthChanged)
    Q_PROPERTY(bool elided READ elided WRITE setElided NOTIFY elidedChanged)
    Q_PROPERTY(qreal padding READ padding NOTIFY paddingChanged)

  public:
    explicit NameFlair(QQuickItem *parent = nullptr);

    [[nodiscard]] QString flairId() const { return m_flairId; }
    void setFlairId(const QString &flairId);
    [[nodiscard]] QString text() const { return m_text; }
    void setText(const QString &text);
    [[nodiscard]] QFont font() const { return m_font; }
    void setFont(const QFont &font);
    [[nodiscard]] QColor textColor() const { return m_textColor; }
    void setTextColor(const QColor &color);
    [[nodiscard]] bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    [[nodiscard]] qreal textWidth() const { return m_textWidth; }
    void setTextWidth(qreal width);
    [[nodiscard]] bool elided() const { return m_elided; }
    void setElided(bool elided);
    [[nodiscard]] qreal padding() const;

    void paint(QPainter *painter) override;

  signals:
    void flairIdChanged();
    void textChanged();
    void fontChanged();
    void textColorChanged();
    void darkModeChanged();
    void textWidthChanged();
    void elidedChanged();
    void paddingChanged();

  private:
    QString m_flairId;
    QString m_text;
    QFont m_font;
    QColor m_textColor = QColor(0x2b, 0x3b, 0x53);
    bool m_darkMode = false;
    qreal m_textWidth = 0.0;
    bool m_elided = false;
};

} // namespace OpenChat
