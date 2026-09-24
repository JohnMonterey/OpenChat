#pragma once

#include <QQuickPaintedItem>

namespace OpenChat {

// The little painted smiley after "Mood:" (SPEC §5.1): a yellow glass face
// whose eyes and mouth follow the mood's expression (Profile::moodFace):
// smile, grin, flat, frown, sleepy or wink. 15 px unless sized otherwise.
class ProfileMoodFace : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(int mood READ mood WRITE setMood NOTIFY moodChanged) // Profile.Mood
    Q_PROPERTY(int face READ face NOTIFY moodChanged)               // Profile.MoodFace

public:
    static constexpr qreal defaultSize = 15;

    explicit ProfileMoodFace(QQuickItem *parent = nullptr);

    [[nodiscard]] int mood() const noexcept { return m_mood; }
    void setMood(int mood);
    [[nodiscard]] int face() const;

    void paint(QPainter *painter) override;

signals:
    void moodChanged();

private:
    int m_mood = 0;
};

} // namespace OpenChat
