#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QRectF>

namespace OpenChat {

// A profile scene: an illustrated backdrop behind the user's own header, in
// the spirit of Messenger's scenes. Each scene has a day (light theme) and a
// night (dark theme) version, fades out along its bottom edge, and softens the
// `readableRect` region so the name and status line stay legible over it.
class ProfileScene : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString sceneId READ sceneId WRITE setSceneId NOTIFY sceneIdChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY darkModeChanged)
    Q_PROPERTY(QRectF readableRect READ readableRect WRITE setReadableRect NOTIFY readableRectChanged)
    // How far up from the bottom edge the scene fades to nothing.
    Q_PROPERTY(qreal fadeHeight READ fadeHeight WRITE setFadeHeight NOTIFY fadeHeightChanged)

  public:
    explicit ProfileScene(QQuickItem *parent = nullptr);

    [[nodiscard]] QString sceneId() const { return m_sceneId; }
    void setSceneId(const QString &sceneId);
    [[nodiscard]] bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool dark);
    [[nodiscard]] QRectF readableRect() const { return m_readableRect; }
    void setReadableRect(const QRectF &rect);
    [[nodiscard]] qreal fadeHeight() const { return m_fadeHeight; }
    void setFadeHeight(qreal height);

    static QImage render(const QString &sceneId, const QSizeF &size, bool dark,
                         const QRectF &readable, qreal fadeHeight, qreal dpr);

    void paint(QPainter *painter) override;

  signals:
    void sceneIdChanged();
    void darkModeChanged();
    void readableRectChanged();
    void fadeHeightChanged();

  private:
    QString m_sceneId;
    bool m_darkMode = false;
    QRectF m_readableRect;
    qreal m_fadeHeight = 14.0;
};

} // namespace OpenChat
