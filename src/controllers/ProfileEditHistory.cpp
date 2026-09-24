#include "controllers/ProfileEditHistory.h"

#include <algorithm>

namespace OpenChat {

ProfileEditHistory::ProfileEditHistory(int capacity)
    : m_capacity(std::max(1, capacity))
{
}

void ProfileEditHistory::clear()
{
    m_undo.clear();
    m_redo.clear();
    m_gesture.reset();
}

void ProfileEditHistory::record(const Profile::Page &before, const Profile::Page &after)
{
    if (before == after)
        return;
    // A new change makes the undone future unreachable.
    m_redo.clear();
    if (m_gesture) {
        // The gesture's one step holds the page from before its first change.
        if (m_gesture->recorded)
            return;
        m_gesture->recorded = true;
    }
    push(m_undo, before);
}

void ProfileEditHistory::beginGesture(const QString &key, const Profile::Page &current)
{
    if (m_gesture && m_gesture->key == key)
        return;
    if (m_gesture)
        endGesture(current);
    m_gesture = Gesture{key, false};
}

void ProfileEditHistory::endGesture(const Profile::Page &current)
{
    if (!m_gesture)
        return;
    // A drag that came back to where it started changed nothing.
    if (m_gesture->recorded && !m_undo.empty() && m_undo.back() == current)
        m_undo.pop_back();
    m_gesture.reset();
}

std::optional<Profile::Page> ProfileEditHistory::undo(const Profile::Page &current)
{
    endGesture(current);
    if (m_undo.empty())
        return std::nullopt;
    Profile::Page previous = std::move(m_undo.back());
    m_undo.pop_back();
    push(m_redo, current);
    return previous;
}

std::optional<Profile::Page> ProfileEditHistory::redo(const Profile::Page &current)
{
    endGesture(current);
    if (m_redo.empty())
        return std::nullopt;
    Profile::Page next = std::move(m_redo.back());
    m_redo.pop_back();
    push(m_undo, current);
    return next;
}

void ProfileEditHistory::push(std::deque<Profile::Page> &stack, const Profile::Page &page)
{
    stack.push_back(page);
    while (int(stack.size()) > m_capacity)
        stack.pop_front();
}

} // namespace OpenChat
