#include "domain/ProfilePageCodec.h"

#include "domain/Identifiers.h"
#include "domain/SongContainer.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace OpenChat {

namespace {

namespace P = Profile;

constexpr char pageTag = '\xFF';
constexpr qsizetype sha256Bytes = 32;
constexpr int maxWantedMedia = 2;
// Only the first entries of a module list are looked at: there are six
// modules today, and a future client with more still fits in sixteen.
constexpr qsizetype maxModuleEntries = 16;

// Keys, as frozen in ARCH §1. Every map is written in ascending key order,
// so an encoding is deterministic; decoders accept any order.
enum MessageKey : qint64 { VersionKey = 0, TypeKey = 1 };
enum class PageType : qint64 { Core = 1, Media = 2, Request = 3 };

namespace CoreKey {
enum : qint64 { Revision = 2, PublishedAt = 3, Theme = 4, Layout = 5, Content = 6, TopFriends = 7, Media = 8,
                Preset = 9 };
}
namespace ThemeKey {
enum : qint64 { BackgroundKind = 1, BackgroundColor1, BackgroundColor2, Motif, MotifInk, MotifOpacity,
                MotifScale, ImageMode, ImageFixed, BoxFill, BoxOpacity, BorderColor, BorderWidth, BorderStyle,
                BoxRadius, HeaderStyle, HeaderFill, HeaderText, AltHeader, AltHeaderFill, AltHeaderText,
                HeadingFont, BodyFont, TextSize, BodyColor, LabelColor, LinkColor, NameFont, NameColor,
                NameColor2, NameEffect, Ambient, Adaptive, NameSize, NameFlourish, TableStyle, BoxGlow,
                AltBorderColor };
static_assert(AltBorderColor == 38);
}
namespace LayoutKey {
enum : qint64 { Layout = 1, Modules = 2 };
}
namespace ModuleKey {
enum : qint64 { Module = 1, Column = 2, Visible = 3 };
}
namespace ContentKey {
enum : qint64 { DisplayName = 1, Headline, InfoLines, Mood, Interests, Details, AboutMe, Meet, SongTitle,
                SongArtist };
static_assert(SongArtist == 10);
}
namespace InterestKey {
enum : qint64 { General = 1, Music, Movies, Television, Books, Heroes };
}
namespace DetailKey {
enum : qint64 { HereFor = 1, Hometown, Zodiac, Occupation, Education, Languages };
}
namespace FriendKey {
// Key 3 (a handle) is reserved and never sent: a viewer takes handles from
// the relay only, never from someone else's page.
enum : qint64 { AccountId = 1, Name = 2 };
}
namespace MediaRefsKey {
enum : qint64 { Background = 1, Song = 2 };
}
namespace RefKey {
enum : qint64 { Sha256 = 1, Bytes = 2, Width = 3, Height = 4, DurationMs = 3 };
}
namespace MediaKey {
enum : qint64 { Kind = 2, Sha256 = 3, Data = 4 };
}
namespace RequestKey {
enum : qint64 { HaveRevision = 2, WantMedia = 3 };
}

// The integer-keyed fields of one CBOR map, looked up by key. Keys that are
// not integers, or larger than any key this version knows, are ignored so a
// newer sender can add fields. An integer key that appears twice makes the
// map invalid: canonical CBOR has no duplicates, and two readers must never
// disagree about which value counts.
class Fields final
{
public:
    static constexpr qint64 maxKnownKey = 63;

    [[nodiscard]] static std::optional<Fields> read(const QCborValue &value)
    {
        if (!value.isMap())
            return std::nullopt;
        const QCborMap map = value.toMap();
        Fields fields;
        std::vector<qint64> keys;
        keys.reserve(static_cast<size_t>(map.size()));
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            const QCborValue key = it.key();
            if (!key.isInteger())
                continue;
            const qint64 number = key.toInteger();
            keys.push_back(number);
            if (number >= 0 && number <= maxKnownKey)
                fields.m_values[static_cast<size_t>(number)] = it.value();
        }
        std::sort(keys.begin(), keys.end());
        if (std::adjacent_find(keys.cbegin(), keys.cend()) != keys.cend())
            return std::nullopt;
        return fields;
    }

    [[nodiscard]] const QCborValue *find(qint64 key) const
    {
        const auto &slot = m_values[static_cast<size_t>(key)];
        return slot ? &*slot : nullptr;
    }

private:
    std::array<std::optional<QCborValue>, maxKnownKey + 1> m_values;
};

// Typed access to one map's fields. An absent field reads as nullopt and
// leaves the caller's default; a known field of the wrong CBOR type also reads
// as nullopt and marks the whole map invalid (ok() == false).
class FieldReader final
{
public:
    explicit FieldReader(const Fields &fields)
        : m_fields(fields)
    {
    }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }

    // A CBOR unsigned integer. QCborValue holds one above INT64_MAX as a
    // double and a negative integer is CBOR's other integer type, so both are
    // the wrong type here; no encoder of this format produces either.
    [[nodiscard]] std::optional<qint64> unsignedValue(qint64 key)
    {
        const QCborValue *value = m_fields.find(key);
        if (!value)
            return std::nullopt;
        if (!value->isInteger() || value->toInteger() < 0)
            return invalid<qint64>();
        return value->toInteger();
    }

    [[nodiscard]] std::optional<bool> boolean(qint64 key)
    {
        const QCborValue *value = m_fields.find(key);
        if (!value)
            return std::nullopt;
        if (!value->isBool())
            return invalid<bool>();
        return value->toBool();
    }

    [[nodiscard]] std::optional<QString> text(qint64 key)
    {
        const QCborValue *value = m_fields.find(key);
        if (!value)
            return std::nullopt;
        if (!value->isString())
            return invalid<QString>();
        return value->toString();
    }

    [[nodiscard]] std::optional<QByteArray> bytes(qint64 key)
    {
        const QCborValue *value = m_fields.find(key);
        if (!value)
            return std::nullopt;
        if (!value->isByteArray())
            return invalid<QByteArray>();
        return value->toByteArray();
    }

    [[nodiscard]] std::optional<QCborValue> map(qint64 key)
    {
        const QCborValue *value = m_fields.find(key);
        if (!value)
            return std::nullopt;
        if (!value->isMap())
            return invalid<QCborValue>();
        return *value;
    }

    [[nodiscard]] std::optional<QCborArray> array(qint64 key)
    {
        const QCborValue *value = m_fields.find(key);
        if (!value)
            return std::nullopt;
        if (!value->isArray())
            return invalid<QCborArray>();
        return value->toArray();
    }

private:
    template<typename T>
    [[nodiscard]] std::optional<T> invalid()
    {
        m_ok = false;
        return std::nullopt;
    }

    const Fields &m_fields;
    bool m_ok = true;
};

// Wire integers are wider than the fields they fill. Anything above 255
// becomes 255, which no enum uses except Preset, where it is CustomPreset:
// the fallback for an unknown preset anyway. normalized() then resets every
// out-of-range enum to its default and clamps the numbers.
template<typename Enum>
[[nodiscard]] Enum enumFromWire(qint64 raw) noexcept
{
    return static_cast<Enum>(static_cast<quint8>(std::min<qint64>(raw, 255)));
}

[[nodiscard]] quint8 smallFromWire(qint64 raw) noexcept
{
    return static_cast<quint8>(std::min<qint64>(raw, 255));
}

[[nodiscard]] quint32 colourFromWire(qint64 raw) noexcept
{
    return static_cast<quint32>(raw & 0xFFFFFF);
}

template<typename Unsigned>
[[nodiscard]] Unsigned saturatedFromWire(qint64 raw) noexcept
{
    return static_cast<Unsigned>(std::min<qint64>(raw, qint64(std::numeric_limits<Unsigned>::max())));
}

template<typename Enum>
[[nodiscard]] qint64 wire(Enum value) noexcept
{
    return static_cast<qint64>(value);
}

[[nodiscard]] QByteArray tagged(const QCborMap &map)
{
    QByteArray payload(1, pageTag);
    payload += QCborValue(map).toCbor();
    return payload;
}

[[nodiscard]] QCborMap messageHeader(PageType type)
{
    QCborMap map;
    map.insert(qint64(VersionKey), qint64(pageWireVersion));
    map.insert(qint64(TypeKey), wire(type));
    return map;
}

struct PageMessage {
    qint64 type = 0;
    Fields fields;
};

// The shared first steps of every decoder: the tag, the largest cap of any
// page message, exactly one well-formed CBOR map after the tag, version 1 and
// a type. Per-type caps are checked by each decoder once it knows the type.
[[nodiscard]] std::optional<PageMessage> readPageMessage(QByteArrayView payload)
{
    if (payload.size() < 2 || payload.size() > maxPageMediaMessageBytes || payload.front() != pageTag)
        return std::nullopt;
    // The view outlives the parse, so the body is read in place; decoded byte
    // strings are copied out by the parser.
    const QByteArray body = QByteArray::fromRawData(payload.data() + 1, payload.size() - 1);
    QCborParserError error;
    const QCborValue root = QCborValue::fromCbor(body, &error);
    if (error.error != QCborError::NoError || error.offset != body.size())
        return std::nullopt;
    auto fields = Fields::read(root);
    if (!fields)
        return std::nullopt;
    FieldReader reader(*fields);
    const auto version = reader.unsignedValue(VersionKey);
    const auto type = reader.unsignedValue(TypeKey);
    if (!reader.ok() || version != qint64(pageWireVersion) || !type)
        return std::nullopt;
    return PageMessage{*type, std::move(*fields)};
}

[[nodiscard]] bool looksLikeJpeg(QByteArrayView data) noexcept
{
    return data.size() >= 3 && quint8(data[0]) == 0xFF && quint8(data[1]) == 0xD8 && quint8(data[2]) == 0xFF;
}

// --- PageCore: encoding -----------------------------------------------------

[[nodiscard]] QCborMap writeTheme(const P::Theme &theme)
{
    QCborMap map;
    const auto colour = [&](qint64 key, quint32 value) { map.insert(key, qint64(value)); };
    const auto number = [&](qint64 key, quint8 value) { map.insert(key, qint64(value)); };
    const auto flag = [&](qint64 key, bool value) { map.insert(key, value); };
    const auto choice = [&](qint64 key, auto value) { map.insert(key, wire(value)); };

    choice(ThemeKey::BackgroundKind, theme.backgroundKind);
    colour(ThemeKey::BackgroundColor1, theme.backgroundColor1);
    colour(ThemeKey::BackgroundColor2, theme.backgroundColor2);
    choice(ThemeKey::Motif, theme.motif);
    colour(ThemeKey::MotifInk, theme.motifInk);
    number(ThemeKey::MotifOpacity, theme.motifOpacity);
    choice(ThemeKey::MotifScale, theme.motifScale);
    choice(ThemeKey::ImageMode, theme.imageMode);
    flag(ThemeKey::ImageFixed, theme.imageFixed);
    colour(ThemeKey::BoxFill, theme.boxFill);
    number(ThemeKey::BoxOpacity, theme.boxOpacity);
    colour(ThemeKey::BorderColor, theme.borderColor);
    number(ThemeKey::BorderWidth, theme.borderWidth);
    choice(ThemeKey::BorderStyle, theme.borderStyle);
    choice(ThemeKey::BoxRadius, theme.boxRadius);
    choice(ThemeKey::HeaderStyle, theme.headerStyle);
    colour(ThemeKey::HeaderFill, theme.headerFill);
    colour(ThemeKey::HeaderText, theme.headerText);
    flag(ThemeKey::AltHeader, theme.altHeader);
    colour(ThemeKey::AltHeaderFill, theme.altHeaderFill);
    colour(ThemeKey::AltHeaderText, theme.altHeaderText);
    choice(ThemeKey::HeadingFont, theme.headingFont);
    choice(ThemeKey::BodyFont, theme.bodyFont);
    choice(ThemeKey::TextSize, theme.textSize);
    colour(ThemeKey::BodyColor, theme.bodyColor);
    colour(ThemeKey::LabelColor, theme.labelColor);
    colour(ThemeKey::LinkColor, theme.linkColor);
    choice(ThemeKey::NameFont, theme.nameFont);
    colour(ThemeKey::NameColor, theme.nameColor);
    colour(ThemeKey::NameColor2, theme.nameColor2);
    choice(ThemeKey::NameEffect, theme.nameEffect);
    choice(ThemeKey::Ambient, theme.ambient);
    flag(ThemeKey::Adaptive, theme.adaptive);
    choice(ThemeKey::NameSize, theme.nameSize);
    choice(ThemeKey::NameFlourish, theme.nameFlourish);
    choice(ThemeKey::TableStyle, theme.tableStyle);
    flag(ThemeKey::BoxGlow, theme.boxGlow);
    colour(ThemeKey::AltBorderColor, theme.altBorderColor);
    return map;
}

[[nodiscard]] QCborMap writeLayout(const P::Page &page)
{
    QCborArray modules;
    for (const P::ModulePlacement &placement : page.modules) {
        QCborMap entry;
        entry.insert(qint64(ModuleKey::Module), wire(placement.module));
        entry.insert(qint64(ModuleKey::Column), wire(placement.column));
        entry.insert(qint64(ModuleKey::Visible), placement.visible);
        modules.append(entry);
    }
    QCborMap map;
    map.insert(qint64(LayoutKey::Layout), wire(page.layout));
    map.insert(qint64(LayoutKey::Modules), modules);
    return map;
}

[[nodiscard]] QCborMap writeContent(const P::Content &content)
{
    QCborMap interests;
    interests.insert(qint64(InterestKey::General), content.interests.general);
    interests.insert(qint64(InterestKey::Music), content.interests.music);
    interests.insert(qint64(InterestKey::Movies), content.interests.movies);
    interests.insert(qint64(InterestKey::Television), content.interests.television);
    interests.insert(qint64(InterestKey::Books), content.interests.books);
    interests.insert(qint64(InterestKey::Heroes), content.interests.heroes);

    QCborMap details;
    details.insert(qint64(DetailKey::HereFor), qint64(content.details.hereFor));
    details.insert(qint64(DetailKey::Hometown), content.details.hometown);
    details.insert(qint64(DetailKey::Zodiac), wire(content.details.zodiac));
    details.insert(qint64(DetailKey::Occupation), content.details.occupation);
    details.insert(qint64(DetailKey::Education), content.details.education);
    details.insert(qint64(DetailKey::Languages), content.details.languages);

    QCborMap map;
    map.insert(qint64(ContentKey::DisplayName), content.displayName);
    map.insert(qint64(ContentKey::Headline), content.headline);
    map.insert(qint64(ContentKey::InfoLines), QCborArray::fromStringList(content.infoLines));
    map.insert(qint64(ContentKey::Mood), wire(content.mood));
    map.insert(qint64(ContentKey::Interests), interests);
    map.insert(qint64(ContentKey::Details), details);
    map.insert(qint64(ContentKey::AboutMe), content.aboutMe);
    map.insert(qint64(ContentKey::Meet), content.meet);
    map.insert(qint64(ContentKey::SongTitle), content.songTitle);
    map.insert(qint64(ContentKey::SongArtist), content.songArtist);
    return map;
}

[[nodiscard]] QCborArray writeTopFriends(const QVector<P::TopFriend> &friends)
{
    QCborArray array;
    for (const P::TopFriend &topFriend : friends) {
        QCborMap entry;
        entry.insert(qint64(FriendKey::AccountId), topFriend.accountId);
        entry.insert(qint64(FriendKey::Name), topFriend.name);
        array.append(entry);
    }
    return array;
}

// Only refs that are set are written; an absent key means "no such media".
[[nodiscard]] QCborMap writeMedia(const P::Page &page)
{
    QCborMap map;
    if (page.background.isSet()) {
        QCborMap ref;
        ref.insert(qint64(RefKey::Sha256), page.background.sha256);
        ref.insert(qint64(RefKey::Bytes), qint64(page.background.bytes));
        ref.insert(qint64(RefKey::Width), qint64(page.background.width));
        ref.insert(qint64(RefKey::Height), qint64(page.background.height));
        map.insert(qint64(MediaRefsKey::Background), ref);
    }
    if (page.song.isSet()) {
        QCborMap ref;
        ref.insert(qint64(RefKey::Sha256), page.song.sha256);
        ref.insert(qint64(RefKey::Bytes), qint64(page.song.bytes));
        ref.insert(qint64(RefKey::DurationMs), qint64(page.song.durationMs));
        map.insert(qint64(MediaRefsKey::Song), ref);
    }
    return map;
}

// --- PageCore: decoding -----------------------------------------------------
// Each reader fills its part of a default Page and returns false only for a
// malformed shape; normalized() repairs values afterwards.

[[nodiscard]] bool readTheme(const QCborValue &value, P::Theme &theme)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    const auto colour = [&](qint64 key, quint32 &out) {
        if (const auto raw = reader.unsignedValue(key))
            out = colourFromWire(*raw);
    };
    const auto number = [&](qint64 key, quint8 &out) {
        if (const auto raw = reader.unsignedValue(key))
            out = smallFromWire(*raw);
    };
    const auto flag = [&](qint64 key, bool &out) {
        if (const auto raw = reader.boolean(key))
            out = *raw;
    };
    const auto choice = [&]<typename Enum>(qint64 key, Enum &out) {
        if (const auto raw = reader.unsignedValue(key))
            out = enumFromWire<Enum>(*raw);
    };

    choice(ThemeKey::BackgroundKind, theme.backgroundKind);
    colour(ThemeKey::BackgroundColor1, theme.backgroundColor1);
    colour(ThemeKey::BackgroundColor2, theme.backgroundColor2);
    choice(ThemeKey::Motif, theme.motif);
    colour(ThemeKey::MotifInk, theme.motifInk);
    number(ThemeKey::MotifOpacity, theme.motifOpacity);
    choice(ThemeKey::MotifScale, theme.motifScale);
    choice(ThemeKey::ImageMode, theme.imageMode);
    flag(ThemeKey::ImageFixed, theme.imageFixed);
    colour(ThemeKey::BoxFill, theme.boxFill);
    number(ThemeKey::BoxOpacity, theme.boxOpacity);
    colour(ThemeKey::BorderColor, theme.borderColor);
    number(ThemeKey::BorderWidth, theme.borderWidth);
    choice(ThemeKey::BorderStyle, theme.borderStyle);
    choice(ThemeKey::BoxRadius, theme.boxRadius);
    choice(ThemeKey::HeaderStyle, theme.headerStyle);
    colour(ThemeKey::HeaderFill, theme.headerFill);
    colour(ThemeKey::HeaderText, theme.headerText);
    flag(ThemeKey::AltHeader, theme.altHeader);
    colour(ThemeKey::AltHeaderFill, theme.altHeaderFill);
    colour(ThemeKey::AltHeaderText, theme.altHeaderText);
    choice(ThemeKey::HeadingFont, theme.headingFont);
    choice(ThemeKey::BodyFont, theme.bodyFont);
    choice(ThemeKey::TextSize, theme.textSize);
    colour(ThemeKey::BodyColor, theme.bodyColor);
    colour(ThemeKey::LabelColor, theme.labelColor);
    colour(ThemeKey::LinkColor, theme.linkColor);
    choice(ThemeKey::NameFont, theme.nameFont);
    colour(ThemeKey::NameColor, theme.nameColor);
    colour(ThemeKey::NameColor2, theme.nameColor2);
    choice(ThemeKey::NameEffect, theme.nameEffect);
    choice(ThemeKey::Ambient, theme.ambient);
    flag(ThemeKey::Adaptive, theme.adaptive);
    choice(ThemeKey::NameSize, theme.nameSize);
    choice(ThemeKey::NameFlourish, theme.nameFlourish);
    choice(ThemeKey::TableStyle, theme.tableStyle);
    flag(ThemeKey::BoxGlow, theme.boxGlow);
    // A sender from before key 38 drew its wide boxes with the main border.
    theme.altBorderColor = theme.borderColor;
    colour(ThemeKey::AltBorderColor, theme.altBorderColor);
    return reader.ok();
}

[[nodiscard]] bool readModules(const QCborArray &array, QVector<P::ModulePlacement> &modules)
{
    const qsizetype count = std::min(array.size(), maxModuleEntries);
    for (qsizetype i = 0; i < count; ++i) {
        const auto fields = Fields::read(array.at(i));
        if (!fields)
            return false;
        FieldReader reader(*fields);
        const auto module = reader.unsignedValue(ModuleKey::Module);
        const auto column = reader.unsignedValue(ModuleKey::Column);
        const auto visible = reader.boolean(ModuleKey::Visible);
        if (!reader.ok())
            return false;
        if (!module)
            continue; // nothing to place
        // An absent column reads as 255, out of range, which normalized()
        // turns into the module's default column.
        modules.push_back({enumFromWire<P::Module>(*module), enumFromWire<P::Column>(column.value_or(255)),
                           visible.value_or(true)});
    }
    return true;
}

[[nodiscard]] bool readLayout(const QCborValue &value, P::Page &page)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    if (const auto layout = reader.unsignedValue(LayoutKey::Layout))
        page.layout = enumFromWire<P::Layout>(*layout);
    const auto modules = reader.array(LayoutKey::Modules);
    if (!reader.ok())
        return false;
    return !modules || readModules(*modules, page.modules);
}

[[nodiscard]] bool readInterests(const QCborValue &value, P::Interests &interests)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    interests.general = reader.text(InterestKey::General).value_or(QString());
    interests.music = reader.text(InterestKey::Music).value_or(QString());
    interests.movies = reader.text(InterestKey::Movies).value_or(QString());
    interests.television = reader.text(InterestKey::Television).value_or(QString());
    interests.books = reader.text(InterestKey::Books).value_or(QString());
    interests.heroes = reader.text(InterestKey::Heroes).value_or(QString());
    return reader.ok();
}

[[nodiscard]] bool readDetails(const QCborValue &value, P::Details &details)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    if (const auto hereFor = reader.unsignedValue(DetailKey::HereFor))
        details.hereFor = static_cast<quint8>(*hereFor & P::hereForMask);
    details.hometown = reader.text(DetailKey::Hometown).value_or(QString());
    if (const auto zodiac = reader.unsignedValue(DetailKey::Zodiac))
        details.zodiac = enumFromWire<P::Zodiac>(*zodiac);
    details.occupation = reader.text(DetailKey::Occupation).value_or(QString());
    details.education = reader.text(DetailKey::Education).value_or(QString());
    details.languages = reader.text(DetailKey::Languages).value_or(QString());
    return reader.ok();
}

[[nodiscard]] bool readContent(const QCborValue &value, P::Content &content)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    content.displayName = reader.text(ContentKey::DisplayName).value_or(QString());
    content.headline = reader.text(ContentKey::Headline).value_or(QString());
    if (const auto lines = reader.array(ContentKey::InfoLines)) {
        for (const QCborValue &line : *lines) {
            if (!line.isString())
                return false;
            content.infoLines.push_back(line.toString());
        }
    }
    if (const auto mood = reader.unsignedValue(ContentKey::Mood))
        content.mood = enumFromWire<P::Mood>(*mood);
    const auto interests = reader.map(ContentKey::Interests);
    const auto details = reader.map(ContentKey::Details);
    content.aboutMe = reader.text(ContentKey::AboutMe).value_or(QString());
    content.meet = reader.text(ContentKey::Meet).value_or(QString());
    content.songTitle = reader.text(ContentKey::SongTitle).value_or(QString());
    content.songArtist = reader.text(ContentKey::SongArtist).value_or(QString());
    if (!reader.ok())
        return false;
    if (interests && !readInterests(*interests, content.interests))
        return false;
    return !details || readDetails(*details, content.details);
}

[[nodiscard]] bool readTopFriends(const QCborArray &array, QVector<P::TopFriend> &friends)
{
    for (const QCborValue &entry : array) {
        const auto fields = Fields::read(entry);
        if (!fields)
            return false;
        FieldReader reader(*fields);
        const auto accountId = reader.bytes(FriendKey::AccountId);
        const auto name = reader.text(FriendKey::Name);
        // The reserved handle key is not read at all, not even its type.
        if (!reader.ok() || !accountId || accountId->size() != AccountId::byteCount)
            return false;
        friends.push_back({*accountId, name.value_or(QString())});
    }
    return true;
}

// A present ref must be a map with a 32-byte hash; sizes out of range are
// kept as read, and normalized() drops just that ref.
[[nodiscard]] bool readMediaRef(const QCborValue &value, bool background, P::MediaRef &ref)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    const auto sha256 = reader.bytes(RefKey::Sha256);
    const auto bytes = reader.unsignedValue(RefKey::Bytes);
    std::optional<qint64> width, height, durationMs;
    if (background) {
        width = reader.unsignedValue(RefKey::Width);
        height = reader.unsignedValue(RefKey::Height);
    } else {
        durationMs = reader.unsignedValue(RefKey::DurationMs);
    }
    if (!reader.ok() || !sha256 || sha256->size() != sha256Bytes)
        return false;
    ref.sha256 = *sha256;
    ref.bytes = saturatedFromWire<quint32>(bytes.value_or(0));
    ref.width = saturatedFromWire<quint16>(width.value_or(0));
    ref.height = saturatedFromWire<quint16>(height.value_or(0));
    ref.durationMs = saturatedFromWire<quint32>(durationMs.value_or(0));
    return true;
}

[[nodiscard]] bool readMedia(const QCborValue &value, P::Page &page)
{
    const auto fields = Fields::read(value);
    if (!fields)
        return false;
    FieldReader reader(*fields);
    const auto background = reader.map(MediaRefsKey::Background);
    const auto song = reader.map(MediaRefsKey::Song);
    if (!reader.ok())
        return false;
    if (background && !readMediaRef(*background, true, page.background))
        return false;
    return !song || readMediaRef(*song, false, page.song);
}

} // namespace

ProfilePayloadKind classifyProfilePayload(QByteArrayView payload)
{
    if (payload.isEmpty() || payload.front() != pageTag)
        return ProfilePayloadKind::Legacy;
    const auto message = readPageMessage(payload);
    if (!message)
        return ProfilePayloadKind::UnknownPage;
    switch (static_cast<PageType>(message->type)) {
    case PageType::Core:
        return ProfilePayloadKind::PageCore;
    case PageType::Media: {
        // A media kind this client does not know is a newer client's blob,
        // not a broken message: ignore it without complaint.
        FieldReader reader(message->fields);
        const auto kind = reader.unsignedValue(MediaKey::Kind);
        if (kind && *kind != wire(P::MediaKind::BackgroundImageMedia) && *kind != wire(P::MediaKind::SongMedia))
            return ProfilePayloadKind::UnknownPage;
        return ProfilePayloadKind::PageMedia;
    }
    case PageType::Request:
        return ProfilePayloadKind::PageRequest;
    }
    return ProfilePayloadKind::UnknownPage;
}

QByteArray encodePageCore(const P::Page &input)
{
    const P::Page page = P::normalized(input);
    QCborMap map = messageHeader(PageType::Core);
    map.insert(qint64(CoreKey::Revision), page.revision);
    map.insert(qint64(CoreKey::PublishedAt), page.publishedAtMs);
    map.insert(qint64(CoreKey::Theme), writeTheme(page.theme));
    map.insert(qint64(CoreKey::Layout), writeLayout(page));
    map.insert(qint64(CoreKey::Content), writeContent(page.content));
    map.insert(qint64(CoreKey::TopFriends), writeTopFriends(page.topFriends));
    map.insert(qint64(CoreKey::Media), writeMedia(page));
    map.insert(qint64(CoreKey::Preset), wire(page.preset));
    return tagged(map);
}

std::optional<P::Page> decodePageCore(QByteArrayView payload)
{
    if (payload.size() > maxPageCoreBytes)
        return std::nullopt;
    const auto message = readPageMessage(payload);
    if (!message || message->type != wire(PageType::Core))
        return std::nullopt;
    FieldReader reader(message->fields);
    P::Page page;
    if (const auto revision = reader.unsignedValue(CoreKey::Revision))
        page.revision = std::min(*revision, P::maxRevision);
    if (const auto publishedAt = reader.unsignedValue(CoreKey::PublishedAt))
        page.publishedAtMs = std::min(*publishedAt, P::maxRevision);
    const auto theme = reader.map(CoreKey::Theme);
    const auto layout = reader.map(CoreKey::Layout);
    const auto content = reader.map(CoreKey::Content);
    const auto topFriends = reader.array(CoreKey::TopFriends);
    const auto media = reader.map(CoreKey::Media);
    if (const auto preset = reader.unsignedValue(CoreKey::Preset))
        page.preset = enumFromWire<P::Preset>(*preset);
    if (!reader.ok())
        return std::nullopt;
    if (theme && !readTheme(*theme, page.theme))
        return std::nullopt;
    if (layout && !readLayout(*layout, page))
        return std::nullopt;
    if (content && !readContent(*content, page.content))
        return std::nullopt;
    if (topFriends && !readTopFriends(*topFriends, page.topFriends))
        return std::nullopt;
    if (media && !readMedia(*media, page))
        return std::nullopt;
    return P::normalized(page);
}

QByteArray encodePageMedia(const PageMediaMessage &message)
{
    Q_ASSERT(message.sha256 == pageMediaHash(message.data));
    QCborMap map = messageHeader(PageType::Media);
    map.insert(qint64(MediaKey::Kind), wire(message.kind));
    map.insert(qint64(MediaKey::Sha256), message.sha256);
    map.insert(qint64(MediaKey::Data), message.data);
    return tagged(map);
}

std::optional<PageMediaMessage> decodePageMedia(QByteArrayView payload)
{
    const auto message = readPageMessage(payload); // already capped at maxPageMediaMessageBytes
    if (!message || message->type != wire(PageType::Media))
        return std::nullopt;
    FieldReader reader(message->fields);
    const auto kind = reader.unsignedValue(MediaKey::Kind);
    const auto sha256 = reader.bytes(MediaKey::Sha256);
    const auto data = reader.bytes(MediaKey::Data);
    if (!reader.ok() || !kind || !sha256 || !data || sha256->size() != sha256Bytes)
        return std::nullopt;

    PageMediaMessage result;
    if (*kind == wire(P::MediaKind::BackgroundImageMedia)) {
        // A JPEG with no scan draws nothing; more than maxJpegScans is a
        // progressive file built to make every viewer's decoder work hard.
        if (data->size() > maxBackgroundImageBytes || !looksLikeJpeg(*data))
            return std::nullopt;
        const int scans = jpegScanCount(*data);
        if (scans < 1 || scans > maxJpegScans)
            return std::nullopt;
        result.kind = P::MediaKind::BackgroundImageMedia;
    } else if (*kind == wire(P::MediaKind::SongMedia)) {
        if (data->size() > maxSongBytes || !decodeSongContainer(*data))
            return std::nullopt;
        result.kind = P::MediaKind::SongMedia;
    } else {
        return std::nullopt;
    }
    if (pageMediaHash(*data) != *sha256)
        return std::nullopt;
    result.sha256 = *sha256;
    result.data = *data;
    return result;
}

QByteArray encodePageRequest(const PageRequestMessage &message)
{
    QCborMap map = messageHeader(PageType::Request);
    if (message.haveRevision)
        map.insert(qint64(RequestKey::HaveRevision), std::clamp<qint64>(*message.haveRevision, 0, P::maxRevision));
    QCborArray wanted;
    QVector<QByteArray> seen;
    for (const QByteArray &hash : message.wantMedia) {
        if (seen.size() == maxWantedMedia)
            break;
        if (hash.size() != sha256Bytes || seen.contains(hash))
            continue;
        seen.push_back(hash);
        wanted.append(hash);
    }
    map.insert(qint64(RequestKey::WantMedia), wanted);
    return tagged(map);
}

std::optional<PageRequestMessage> decodePageRequest(QByteArrayView payload)
{
    if (payload.size() > maxPageRequestBytes)
        return std::nullopt;
    const auto message = readPageMessage(payload);
    if (!message || message->type != wire(PageType::Request))
        return std::nullopt;
    FieldReader reader(message->fields);
    PageRequestMessage result;
    if (const auto revision = reader.unsignedValue(RequestKey::HaveRevision))
        result.haveRevision = std::min(*revision, P::maxRevision);
    const auto wanted = reader.array(RequestKey::WantMedia);
    if (!reader.ok())
        return std::nullopt;
    if (wanted) {
        for (const QCborValue &entry : *wanted) {
            if (!entry.isByteArray() || entry.toByteArray().size() != sha256Bytes)
                return std::nullopt;
            const QByteArray hash = entry.toByteArray();
            if (result.wantMedia.size() < maxWantedMedia && !result.wantMedia.contains(hash))
                result.wantMedia.push_back(hash);
        }
    }
    return result;
}

QByteArray pageMediaHash(QByteArrayView data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

int jpegScanCount(QByteArrayView jpeg)
{
    const auto byteAt = [&](qsizetype index) { return static_cast<quint8>(jpeg[index]); };
    const qsizetype size = jpeg.size();
    if (size < 4 || byteAt(0) != 0xFF || byteAt(1) != 0xD8)
        return -1;

    constexpr quint8 startOfScan = 0xDA, endOfImage = 0xD9, startOfImage = 0xD8;
    const auto isRestart = [](quint8 marker) { return marker >= 0xD0 && marker <= 0xD7; };

    int scans = 0;
    qsizetype pos = 2;
    while (true) {
        // A marker: 0xFF, any number of 0xFF fill bytes, then the code.
        if (pos >= size || byteAt(pos) != 0xFF)
            return -1;
        while (pos < size && byteAt(pos) == 0xFF)
            ++pos;
        if (pos >= size)
            return -1;
        const quint8 marker = byteAt(pos++);
        if (marker == endOfImage)
            return scans;
        if (marker == startOfImage || marker == 0x00)
            return -1;
        if (isRestart(marker) || marker == 0x01) // standalone markers carry no length
            continue;

        // A segment: a big-endian length that counts itself.
        if (pos + 2 > size)
            return -1;
        const qsizetype length = (qsizetype(byteAt(pos)) << 8) | byteAt(pos + 1);
        if (length < 2 || pos + length > size)
            return -1;
        pos += length;
        if (marker != startOfScan)
            continue;

        // Entropy-coded data follows a scan header until the next real
        // marker: 0xFF00 is a stuffed data byte and 0xFFD0–D7 are restart
        // markers inside the scan.
        ++scans;
        while (pos < size) {
            if (byteAt(pos) != 0xFF) {
                ++pos;
                continue;
            }
            if (pos + 1 >= size)
                return -1;
            const quint8 next = byteAt(pos + 1);
            if (next == 0x00 || isRestart(next))
                pos += 2;
            else if (next == 0xFF)
                ++pos; // fill byte before a marker
            else
                break;
        }
    }
}

} // namespace OpenChat
