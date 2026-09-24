#include "profile/ProfileMoodFaceItem.h"

#include "domain/ProfilePage.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace OpenChat {

namespace {

using Profile::MoodFace;

// The face's own palette: a glossy yellow smiley, like the mockups', and
// the same on every theme (it sits on the box like an emoji would).
const QColor faceTop(0xFF, 0xF1, 0xA8);
const QColor faceMiddle(0xFF, 0xD2, 0x3F);
const QColor faceBottom(0xF2, 0xA9, 0x00);
const QColor faceRim(0xB9, 0x7D, 0x00);
const QColor faceInk(0x5A, 0x3A, 0x00);

} // namespace

ProfileMoodFace::ProfileMoodFace(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setImplicitSize(defaultSize, defaultSize);
}

void ProfileMoodFace::setMood(int mood)
{
    if (mood < 0 || mood > Profile::maxMood || mood == m_mood)
        return;
    m_mood = mood;
    emit moodChanged();
    update();
}

int ProfileMoodFace::face() const
{
    return int(Profile::moodFace(Profile::Mood(m_mood)));
}

void ProfileMoodFace::paint(QPainter *painter)
{
    if (m_mood == int(Profile::Mood::NoMood))
        return;
    const qreal side = std::min(width(), height());
    if (side <= 0)
        return;
    QPainter &p = *painter;
    p.setRenderHint(QPainter::Antialiasing, true);
    // Everything below is drawn in a unit square.
    p.translate((width() - side) / 2, (height() - side) / 2);
    p.scale(side, side);
    const qreal hairline = 1.0 / side; // one logical pixel

    QLinearGradient glass(0, 0.04, 0, 0.96);
    glass.setColorAt(0, faceTop);
    glass.setColorAt(0.5, faceMiddle);
    glass.setColorAt(1, faceBottom);
    p.setPen(QPen(faceRim, hairline));
    p.setBrush(glass);
    p.drawEllipse(QPointF(0.5, 0.5), 0.46, 0.46);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 140));
    p.drawEllipse(QPointF(0.5, 0.27), 0.27, 0.13);

    const qreal stroke = std::max(0.08, 1.1 * hairline);
    const QPen line(faceInk, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const auto eye = [&](qreal x) {
        p.setPen(Qt::NoPen);
        p.setBrush(faceInk);
        p.drawEllipse(QPointF(x, 0.42), 0.065, 0.075);
    };
    const auto smile = [&] {
        QPainterPath mouth;
        mouth.moveTo(0.3, 0.6);
        mouth.quadTo(0.5, 0.8, 0.7, 0.6);
        p.strokePath(mouth, line);
    };

    switch (Profile::moodFace(Profile::Mood(m_mood))) {
    case MoodFace::SmileFace:
        eye(0.35);
        eye(0.65);
        smile();
        break;
    case MoodFace::GrinFace: {
        eye(0.35);
        eye(0.65);
        QPainterPath mouth;
        mouth.moveTo(0.27, 0.57);
        mouth.cubicTo(0.3, 0.86, 0.7, 0.86, 0.73, 0.57);
        mouth.closeSubpath();
        p.fillPath(mouth, faceInk);
        p.fillRect(QRectF(0.33, 0.585, 0.34, 0.07), QColor(255, 255, 255, 220)); // teeth
        break;
    }
    case MoodFace::FlatFace:
        eye(0.35);
        eye(0.65);
        p.strokePath([] {
            QPainterPath mouth;
            mouth.moveTo(0.33, 0.67);
            mouth.lineTo(0.67, 0.67);
            return mouth;
        }(), line);
        break;
    case MoodFace::FrownFace: {
        eye(0.35);
        eye(0.65);
        QPainterPath mouth;
        mouth.moveTo(0.32, 0.73);
        mouth.quadTo(0.5, 0.55, 0.68, 0.73);
        p.strokePath(mouth, line);
        break;
    }
    case MoodFace::SleepyFace: {
        QPainterPath lids;
        lids.moveTo(0.26, 0.42);
        lids.quadTo(0.35, 0.49, 0.44, 0.42);
        lids.moveTo(0.56, 0.42);
        lids.quadTo(0.65, 0.49, 0.74, 0.42);
        p.strokePath(lids, line);
        p.setPen(QPen(faceInk, stroke * 0.8));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(0.5, 0.68), 0.065, 0.075);
        break;
    }
    case MoodFace::WinkFace: {
        eye(0.35);
        QPainterPath wink;
        wink.moveTo(0.56, 0.44);
        wink.lineTo(0.65, 0.37);
        wink.lineTo(0.74, 0.44);
        p.strokePath(wink, line);
        smile();
        break;
    }
    }
}

} // namespace OpenChat
