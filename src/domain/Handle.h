#pragma once

#include <QString>

#include <optional>

namespace OpenChat {

// The single definition of what a username (@handle) is. Shared by the client
// and the relay so both sides agree on the canonical form that uniqueness, login
// and the password-key salt are all computed over.
//
// A canonical handle is 3-32 characters of lowercase ASCII letters, digits and
// `.`, `_`, `-`, starting with a letter or digit. Restricting the alphabet to
// ASCII is deliberate: it makes "unique" mean visually unique, because no two
// distinct canonical handles can be confusable Unicode look-alikes.
inline constexpr qsizetype minimumHandleLength = 3;
inline constexpr qsizetype maximumHandleLength = 32;

// Canonicalizes user input: trims surrounding whitespace, drops one leading `@`
// and lowercases. Returns nullopt when the result is not a canonical handle, so
// a caller can never store or look up a non-canonical form by accident.
[[nodiscard]] std::optional<QString> normalizeHandle(const QString &input);

// True when `handle` is already in canonical form (no normalization applied).
[[nodiscard]] bool isCanonicalHandle(const QString &handle);

} // namespace OpenChat
