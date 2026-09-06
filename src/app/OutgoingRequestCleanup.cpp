#include "app/OutgoingRequestCleanup.h"

#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "diagnostics/Logging.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"

namespace OpenChat {

bool discardOutgoingRequest(ProfileSession &session, const AccountId &peer,
                            const ConversationId &conversation, bool removeContactRow)
{
    bool clean = true;
    if (removeContactRow) {
        auto *contacts = session.contacts();
        if (!contacts || !contacts->remove(peer).hasValue()) {
            qCWarning(contactsLog) << "Could not remove the pending contact row";
            clean = false;
        }
    }
    auto *chats = session.chats();
    if (!chats || !chats->removeConversation(conversation).hasValue()) {
        qCWarning(contactsLog) << "Could not remove the request's conversation";
        clean = false;
    }
    // A group that was never created (preparation failed before that step) is
    // not an error worth reporting; anything else is.
    auto *mls = session.mls();
    if (!mls) {
        clean = false;
    } else if (const auto deleted = mls->deleteGroup(conversation);
               !deleted.hasValue() && deleted.error() != MlsError::MissingGroup) {
        qCWarning(mlsLog) << "Could not delete the request's MLS group";
        clean = false;
    } else if (!session.persistMlsState().hasValue()) {
        qCWarning(mlsLog) << "Could not persist MLS state after deleting a group";
        clean = false;
    }
    return clean;
}

} // namespace OpenChat
