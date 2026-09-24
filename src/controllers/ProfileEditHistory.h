#pragma once

#include "domain/ProfilePage.h"

#include <QString>

#include <deque>
#include <optional>

namespace OpenChat {

// The profile editor's undo history: whole-page snapshots, at most `capacity`
// steps back (the oldest is forgotten first).
//
// Every change to the draft outside a gesture is one step. Inside
// beginGesture(key) … endGesture() every change merges into one step, so a
// slider or colour drag, a whole colour-picker session or a text field's
// focus period undoes in one go. A gesture that ends where it began leaves
// no step behind. Snapshots are taken before each change, so undo() returns
// the page to restore and remembers the current one for redo().
class ProfileEditHistory final
{
public:
    static constexpr int defaultCapacity = 100;

    explicit ProfileEditHistory(int capacity = defaultCapacity);

    // Forgets every step and any open gesture.
    void clear();

    // The draft went from `before` to `after` (the caller skips no-ops).
    void record(const Profile::Page &before, const Profile::Page &after);

    // Opens a gesture. The same key while it is open continues it; another
    // key closes it and opens a new one.
    void beginGesture(const QString &key, const Profile::Page &current);
    // Closes the open gesture; `current` is the draft at its end.
    void endGesture(const Profile::Page &current);
    [[nodiscard]] bool inGesture() const noexcept { return m_gesture.has_value(); }
    [[nodiscard]] QString gestureKey() const { return m_gesture ? m_gesture->key : QString(); }

    [[nodiscard]] bool canUndo() const noexcept { return !m_undo.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }
    [[nodiscard]] int undoDepth() const noexcept { return int(m_undo.size()); }
    // The page to restore, or nullopt when there is nothing to undo. Closes
    // an open gesture first, so a step is never undone half-way.
    [[nodiscard]] std::optional<Profile::Page> undo(const Profile::Page &current);
    [[nodiscard]] std::optional<Profile::Page> redo(const Profile::Page &current);

private:
    struct Gesture final {
        QString key;
        bool recorded = false; // this gesture has pushed its step
    };

    void push(std::deque<Profile::Page> &stack, const Profile::Page &page);

    int m_capacity;
    std::deque<Profile::Page> m_undo; // oldest first
    std::deque<Profile::Page> m_redo;
    std::optional<Gesture> m_gesture;
};

} // namespace OpenChat
