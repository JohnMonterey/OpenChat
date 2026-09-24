#pragma once

#include "domain/Identifiers.h"
#include "domain/ProfilePage.h"

#include <QString>
#include <QStringList>

#include <optional>

// The profile pages of the reference mock (the design's final mockups,
// final/qml/People.js), for everything that renders without a session: the
// --capture and --profile previews, the gallery and the QML tests.
//
// Mock identity is deterministic, so a mock page's Top Friends can name real
// mock people: an id's account is the first 16 bytes of
// SHA-256("openchat-mock:" + id), and mockIdFor() maps it back for every id
// this file knows (the mock roster, "self" and every Friend Space stranger).
namespace OpenChat::ProfileReferencePages {

// The mock local user's id; its account is the viewer's own in mock mode.
[[nodiscard]] QString selfId();
[[nodiscard]] AccountId mockAccountFor(const QString &id);
// The id whose mock account this is, or "" when this file does not know it.
[[nodiscard]] QString mockIdFor(const AccountId &account);
// The mock roster, the same six people ChatController's reference contacts are.
[[nodiscard]] QStringList rosterIds();

// The page a mock contact published: michael Aero Sky, jessica Scene Queen,
// ryan Classic '06, sarah Glitter Girl, alex Midnight Emo. Everyone else
// (tom) has none, so their profile is the default page.
[[nodiscard]] std::optional<Profile::Page> seededPage(const QString &contactId);
// The mock contact whose seeded page uses `preset`, or "" when none does.
[[nodiscard]] QString contactForPreset(Profile::Preset preset);
// Daniel's band page (Headliner), the mockups' "your own page".
[[nodiscard]] Profile::Page ownReferencePage();

} // namespace OpenChat::ProfileReferencePages
