#pragma once

#include "domain/Identifiers.h"

#include <QHash>
#include <QQueue>

#include <functional>

namespace OpenChat {

// Serializes operations per conversation so that two operations never mutate one
// MLS group concurrently or re-entrantly.
//
// The client runs on a single (Qt event-loop) thread, so the hazard this guards
// against is re-entrancy: an incoming-envelope handler and a send for the same
// conversation interleaving if a step spins the event loop. Tasks are run to
// completion one at a time within a conversation lane (FIFO); tasks in different
// conversations are independent. A task that enqueues more work for its own
// conversation (directly or transitively) does not recurse — the new work is
// queued and drained after the current task returns.
class MlsTransactionCoordinator final
{
public:
    using Task = std::function<void()>;

    // Runs task within the conversation's lane. If the lane is idle, the task
    // runs immediately (and then drains any work queued during it); otherwise it
    // is queued behind the in-flight task.
    void run(const ConversationId &conversation, Task task);

    [[nodiscard]] bool isBusy(const ConversationId &conversation) const;
    [[nodiscard]] int pendingCount(const ConversationId &conversation) const;

private:
    struct Lane final {
        bool running = false;
        QQueue<Task> pending;
    };

    QHash<ConversationId, Lane> m_lanes;
};

} // namespace OpenChat
