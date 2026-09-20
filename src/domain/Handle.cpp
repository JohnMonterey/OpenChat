#include "domain/Handle.h"

namespace OpenChat {

namespace {

bool isHandleLetterOrDigit(QChar ch)
{
    const char16_t c = ch.unicode();
    return (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
}

bool isHandleCharacter(QChar ch)
{
    const char16_t c = ch.unicode();
    return isHandleLetterOrDigit(ch) || c == u'.' || c == u'_' || c == u'-';
}

} // namespace

bool isCanonicalHandle(const QString &handle)
{
    if (handle.size() < minimumHandleLength || handle.size() > maximumHandleLength)
        return false;
    if (!isHandleLetterOrDigit(handle.front()))
        return false;
    for (const QChar ch : handle) {
        if (!isHandleCharacter(ch))
            return false;
    }
    return true;
}

std::optional<QString> normalizeHandle(const QString &input)
{
    QString candidate = input.trimmed();
    if (candidate.startsWith(QLatin1Char('@')))
        candidate.remove(0, 1);
    // Only ASCII capitals fold: any other cased letter is outside the alphabet and
    // is rejected below, so a locale- or Unicode-aware fold could only ever turn an
    // invalid handle into a valid-looking one.
    for (QChar &ch : candidate) {
        const char16_t c = ch.unicode();
        if (c >= u'A' && c <= u'Z')
            ch = QChar(static_cast<char16_t>(c - u'A' + u'a'));
    }
    if (!isCanonicalHandle(candidate))
        return std::nullopt;
    return candidate;
}

} // namespace OpenChat
