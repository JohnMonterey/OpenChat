#include "cosmetics/CosmeticCatalog.h"

#include "cosmetics/BubbleSkins.h"

#include <QVariantMap>

#include <array>

namespace OpenChat {

namespace {

struct TierInfo
{
    Rarity rarity;
    const char *id;
    const char *name;
    QRgb color;
    int weight; // out of CosmeticCatalog::totalWeight
};

// The ladder and its odds. The colours are the familiar crate-grade ones (blue,
// purple, pink, red, gold); the weights are gentler than a paid crate's because
// the case is free and daily: something Epic or better about every week.
constexpr std::array<TierInfo, 5> kTiers = {{
    {Rarity::Common, "common", "Common", 0xff4b69ff, 600},
    {Rarity::Rare, "rare", "Rare", 0xff8847ff, 250},
    {Rarity::Epic, "epic", "Epic", 0xffd32ce6, 100},
    {Rarity::Legendary, "legendary", "Legendary", 0xffeb4b4b, 40},
    {Rarity::Exotic, "exotic", "Exotic", 0xffe4ae39, 10},
}};

constexpr int weightSum()
{
    int sum = 0;
    for (const TierInfo &tier : kTiers)
        sum += tier.weight;
    return sum;
}
static_assert(weightSum() == CosmeticCatalog::totalWeight, "tier weights must cover every roll");

struct RarityEntry
{
    const char *id;
    Rarity rarity;
};

// Every item's tier, in one place so the ladder can be balanced at a glance.
// Small or plain items are Common; the rarer an item, the more of the screen it
// takes, the more it does, or both. Animated items sit at the top. Every
// catalogue id must appear exactly once (tst_cosmetics checks).
constexpr RarityEntry kRarityTable[] = {
    // Common: understated, or small enough to be a gentle first drop.
    {"bubble.aero", Rarity::Common},
    {"frame.aero", Rarity::Common},
    {"frame.pixel", Rarity::Common},
    {"bead.heart", Rarity::Common},
    {"bead.star", Rarity::Common},
    {"bead.orb", Rarity::Common},
    {"flair.aero", Rarity::Common},
    {"flair.gold", Rarity::Common},
    // Rare: a clear material, or a whole scene with a calm palette.
    {"frame.neon", Rarity::Rare},
    {"bead.gem", Rarity::Rare},
    {"bead.planet", Rarity::Rare},
    {"bead.flame", Rarity::Rare},
    {"flair.neon", Rarity::Rare},
    {"scene.meadow", Rarity::Rare},
    {"scene.aqua", Rarity::Rare},
    // Epic: detailed craft that reads from across the room.
    {"bubble.marble", Rarity::Epic},
    {"frame.gilded", Rarity::Epic},
    {"frame.frost", Rarity::Epic},
    {"flair.chrome", Rarity::Epic},
    {"flair.holo", Rarity::Epic},
    {"flair.ember", Rarity::Epic},
    {"scene.sakura", Rarity::Epic},
    // Legendary: showpieces on the largest surfaces, or in motion.
    {"bubble.nebula", Rarity::Legendary},
    {"bubble.holo", Rarity::Legendary},
    {"frame.orbit", Rarity::Legendary},
    {"scene.aurora", Rarity::Legendary},
    {"scene.synthwave", Rarity::Legendary},
    // Exotic: the two fire pieces, one on every message, one alive.
    {"bubble.magma", Rarity::Exotic},
    {"frame.inferno", Rarity::Exotic},
};

const TierInfo &tier(Rarity rarity)
{
    return kTiers[static_cast<std::size_t>(rarity)];
}

Rarity rarityOf(const QString &id)
{
    for (const RarityEntry &entry : kRarityTable) {
        if (id == QLatin1String(entry.id))
            return entry.rarity;
    }
    Q_ASSERT_X(false, "CosmeticCatalog", "an item is missing from the rarity table");
    return Rarity::Common;
}

QList<CosmeticInfo> buildCatalogue()
{
    QList<CosmeticInfo> items;
    // Chat-bubble skins: the local user's own messages wear the equipped one.
    for (const BubbleSkinInfo &skin : BubbleSkins::catalog())
        items.append({skin.id, QStringLiteral("bubble"), skin.name, skin.description, false});

    items.append({
        // Avatar frames: drawn around the display picture, never over its middle.
        {QStringLiteral("frame.aero"), QStringLiteral("frame"), QStringLiteral("Aero Glass"),
         QStringLiteral("A tinted glass bezel with a gloss horizon and light streaks."), false},
        {QStringLiteral("frame.gilded"), QStringLiteral("frame"), QStringLiteral("Gilded Filigree"),
         QStringLiteral("Polished gold with beading, scrollwork corners and a ruby."), false},
        {QStringLiteral("frame.neon"), QStringLiteral("frame"), QStringLiteral("Neon Tubes"),
         QStringLiteral("Two bent glass tubes, hot pink and ice cyan, with bloom."), false},
        {QStringLiteral("frame.pixel"), QStringLiteral("frame"), QStringLiteral("8-Bit Hero"),
         QStringLiteral("A chunky pixel-art bevel with rivets and an extra life."), false},
        {QStringLiteral("frame.frost"), QStringLiteral("frame"), QStringLiteral("Frostbite"),
         QStringLiteral("Faceted ice, a snow cap, frost ferns and a snowflake."), false},
        {QStringLiteral("frame.inferno"), QStringLiteral("frame"), QStringLiteral("Inferno"),
         QStringLiteral("Cracked lava rock wreathed in flickering flames and embers."), true},
        {QStringLiteral("frame.orbit"), QStringLiteral("frame"), QStringLiteral("Stardust Orbit"),
         QStringLiteral("A deep-space band with two comets chasing round the picture."), true},

        // Presence beads: the state colour always reads; the material changes.
        {QStringLiteral("bead.gem"), QStringLiteral("bead"), QStringLiteral("Brilliant Cut"),
         QStringLiteral("A faceted gemstone: emerald, citrine, ruby or smoky quartz."), false},
        {QStringLiteral("bead.heart"), QStringLiteral("bead"), QStringLiteral("Candy Heart"),
         QStringLiteral("A glossy hard-candy heart."), false},
        {QStringLiteral("bead.star"), QStringLiteral("bead"), QStringLiteral("Lucky Star"),
         QStringLiteral("A bevelled five-point star with a glint."), false},
        {QStringLiteral("bead.orb"), QStringLiteral("bead"), QStringLiteral("Plasma Orb"),
         QStringLiteral("A glass sphere with a glowing core and halo."), false},
        {QStringLiteral("bead.planet"), QStringLiteral("bead"), QStringLiteral("Ringed Planet"),
         QStringLiteral("A banded gas giant with a tilted ring."), false},
        {QStringLiteral("bead.flame"), QStringLiteral("bead"), QStringLiteral("Spirit Flame"),
         QStringLiteral("A tiny will-o'-the-wisp flame."), false},

        // Name flair: the name keeps its font, size and elision; only the ink changes.
        {QStringLiteral("flair.aero"), QStringLiteral("flair"), QStringLiteral("Aero Glow"),
         QStringLiteral("The Vista caption glow: a soft halo that lifts the name."), false},
        {QStringLiteral("flair.chrome"), QStringLiteral("flair"), QStringLiteral("Y2K Chrome"),
         QStringLiteral("Mirror chrome with a hard horizon line and an outline."), false},
        {QStringLiteral("flair.gold"), QStringLiteral("flair"), QStringLiteral("Gold Leaf"),
         QStringLiteral("Embossed gold leaf with a bright top edge."), false},
        {QStringLiteral("flair.holo"), QStringLiteral("flair"), QStringLiteral("Holo Foil"),
         QStringLiteral("Iridescent foil that shifts through the spectrum, with glints."), false},
        {QStringLiteral("flair.neon"), QStringLiteral("flair"), QStringLiteral("Neon Sign"),
         QStringLiteral("A glowing tube-lit name with a white-hot core."), false},
        {QStringLiteral("flair.ember"), QStringLiteral("flair"), QStringLiteral("Molten"),
         QStringLiteral("White-hot to deep red, with a heat glow and sparks."), false},

        // Profile scenes: a backdrop behind the user's own header, as Messenger had.
        {QStringLiteral("scene.aurora"), QStringLiteral("scene"), QStringLiteral("Northern Lights"),
         QStringLiteral("Aurora curtains over a pine ridge under the stars."), false},
        {QStringLiteral("scene.meadow"), QStringLiteral("scene"), QStringLiteral("Spring Meadow"),
         QStringLiteral("Rolling green hills, a glossy sky and drifting clouds."), false},
        {QStringLiteral("scene.aqua"), QStringLiteral("scene"), QStringLiteral("Aqua Bubbles"),
         QStringLiteral("Aero light ribbons and glassy bubbles."), false},
        {QStringLiteral("scene.synthwave"), QStringLiteral("scene"), QStringLiteral("Outrun Sunset"),
         QStringLiteral("A striped sun sinking behind a neon grid."), false},
        {QStringLiteral("scene.sakura"), QStringLiteral("scene"), QStringLiteral("Sakura Breeze"),
         QStringLiteral("A blossoming branch shedding petals."), false},
    });

    for (CosmeticInfo &info : items)
        info.rarity = rarityOf(info.id);
    return items;
}

// Each tier's members, pointing into all(), for draw().
const std::array<QList<const CosmeticInfo *>, 5> &tierMembers()
{
    static const auto members = [] {
        std::array<QList<const CosmeticInfo *>, 5> lists;
        for (const CosmeticInfo &info : CosmeticCatalog::all())
            lists[static_cast<std::size_t>(info.rarity)].append(&info);
        return lists;
    }();
    return members;
}

} // namespace

const QList<CosmeticInfo> &CosmeticCatalog::all()
{
    static const QList<CosmeticInfo> items = buildCatalogue();
    return items;
}

QList<CosmeticInfo> CosmeticCatalog::inCategory(const QString &category)
{
    QList<CosmeticInfo> result;
    for (const CosmeticInfo &info : all()) {
        if (info.category == category)
            result.append(info);
    }
    return result;
}

const CosmeticInfo *CosmeticCatalog::find(const QString &id)
{
    for (const CosmeticInfo &info : all()) {
        if (info.id == id)
            return &info;
    }
    return nullptr;
}

bool CosmeticCatalog::isKnown(const QString &id, const QString &category)
{
    const CosmeticInfo *info = find(id);
    return info && info->category == category;
}

QList<CosmeticInfo> CosmeticCatalog::ofRarity(Rarity rarity)
{
    QList<CosmeticInfo> result;
    for (const CosmeticInfo &info : all()) {
        if (info.rarity == rarity)
            result.append(info);
    }
    return result;
}

QList<Rarity> CosmeticCatalog::rarities()
{
    QList<Rarity> list;
    for (const TierInfo &entry : kTiers)
        list.append(entry.rarity);
    return list;
}

QString CosmeticCatalog::rarityId(Rarity rarity)
{
    return QString::fromLatin1(tier(rarity).id);
}

QString CosmeticCatalog::rarityName(Rarity rarity)
{
    return QString::fromLatin1(tier(rarity).name);
}

QColor CosmeticCatalog::rarityColor(Rarity rarity)
{
    return QColor::fromRgba(tier(rarity).color);
}

int CosmeticCatalog::rarityWeight(Rarity rarity)
{
    return tier(rarity).weight;
}

const CosmeticInfo &CosmeticCatalog::draw(quint32 tierRoll, quint32 itemRoll)
{
    int roll = static_cast<int>(tierRoll % static_cast<quint32>(totalWeight));
    for (const TierInfo &entry : kTiers) {
        if (roll < entry.weight) {
            const QList<const CosmeticInfo *> &members =
                tierMembers()[static_cast<std::size_t>(entry.rarity)];
            Q_ASSERT(!members.isEmpty());
            if (members.isEmpty())
                break;
            return *members[static_cast<qsizetype>(itemRoll % static_cast<quint32>(members.size()))];
        }
        roll -= entry.weight;
    }
    return all().first();
}

QVariantMap cosmeticToVariant(const CosmeticInfo *info)
{
    if (!info)
        return {};
    return {{QStringLiteral("id"), info->id},
            {QStringLiteral("category"), info->category},
            {QStringLiteral("name"), info->name},
            {QStringLiteral("description"), info->description},
            {QStringLiteral("animated"), info->animated},
            {QStringLiteral("rarity"), CosmeticCatalog::rarityId(info->rarity)},
            {QStringLiteral("rarityRank"), static_cast<int>(info->rarity)},
            {QStringLiteral("rarityName"), CosmeticCatalog::rarityName(info->rarity)},
            {QStringLiteral("rarityColor"), CosmeticCatalog::rarityColor(info->rarity)}};
}

CosmeticsCatalogObject::CosmeticsCatalogObject(QObject *parent) : QObject(parent) {}

QVariantList CosmeticsCatalogObject::items(const QString &category) const
{
    QVariantList list;
    for (const CosmeticInfo &info : CosmeticCatalog::all()) {
        if (category.isEmpty() || info.category == category)
            list.append(cosmeticToVariant(&info));
    }
    return list;
}

QVariantMap CosmeticsCatalogObject::item(const QString &id) const
{
    return cosmeticToVariant(CosmeticCatalog::find(id));
}

QString CosmeticsCatalogObject::displayName(const QString &id) const
{
    const CosmeticInfo *info = CosmeticCatalog::find(id);
    return info ? info->name : QString();
}

QVariantList CosmeticsCatalogObject::tiers() const
{
    QVariantList list;
    for (const TierInfo &entry : kTiers) {
        list.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(entry.id)},
                                {QStringLiteral("name"), QString::fromLatin1(entry.name)},
                                {QStringLiteral("color"), QColor::fromRgba(entry.color)},
                                {QStringLiteral("weight"), entry.weight},
                                {QStringLiteral("percent"),
                                 100.0 * entry.weight / CosmeticCatalog::totalWeight}});
    }
    return list;
}

} // namespace OpenChat
