#pragma once

#include "domain/Identifiers.h"

namespace OpenChat {

class ProfileSession;

// Removes every local trace of an outgoing contact request: the roster row (if
// asked), the conversation it was going to live in, and the MLS group whose
// Welcome was — or was never — sent.
//
// Two callers share it. AddContactService rolls back a request whose key-package
// claim failed after the local rows were written, so a failed add leaves nothing
// behind that reads as "request sent". ContactController withdraws a request the
// user no longer wants to wait on, so the handle can be asked again. A Welcome
// that did reach the peer cannot be recalled; if they accept after this, their
// reply names a conversation this side no longer knows and is ignored.
//
// Best effort, in the order given: a failure in one step is logged and the next
// step still runs, so a hiccup deleting one row cannot leave all three behind.
// Returns true only when every step succeeded.
bool discardOutgoingRequest(ProfileSession &session, const AccountId &peer,
                            const ConversationId &conversation, bool removeContactRow);

} // namespace OpenChat
