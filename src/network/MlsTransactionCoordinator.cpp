#include "network/MlsTransactionCoordinator.h"

#include <utility>

namespace OpenChat {

void MlsTransactionCoordinator::run(const ConversationId &conversation, Task task)
{
    if (!task)
        return;

    // If a task is already in flight for this conversation, queue behind it.
    // (Re-entrant calls from within the running task land here.)
    if (m_lanes.contains(conversation) && m_lanes[conversation].running) {
        m_lanes[conversation].pending.enqueue(std::move(task));
        return;
    }

    m_lanes[conversation].running = true;
    Task current = std::move(task);
    for (;;) {
        current();
        // Re-fetch each iteration: running `current` may have inserted lanes for
        // other conversations and rehashed the container, so never hold a Lane&
        // across the call.
        if (!m_lanes.contains(conversation))
            break; // defensive; should not happen
        if (m_lanes[conversation].pending.isEmpty()) {
            m_lanes.remove(conversation);
            break;
        }
        current = m_lanes[conversation].pending.dequeue();
    }
}

bool MlsTransactionCoordinator::isBusy(const ConversationId &conversation) const
{
    const auto it = m_lanes.constFind(conversation);
    return it != m_lanes.cend() && it->running;
}

int MlsTransactionCoordinator::pendingCount(const ConversationId &conversation) const
{
    const auto it = m_lanes.constFind(conversation);
    return it == m_lanes.cend() ? 0 : static_cast<int>(it->pending.size());
}

} // namespace OpenChat
