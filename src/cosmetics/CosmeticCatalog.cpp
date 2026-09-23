#include "cosmetics/CosmeticCatalog.h"

#include "cosmetics/BubbleSkins.h"

#include <QHash>
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
};

// How the ladder looks: the familiar crate-grade colours (blue, purple, pink,
// red, gold). Its odds are the shared rules' (domain/CosmeticRules).
constexpr std::array<TierInfo, 5> kTiers = {{
    {Rarity::Common, "common", "Common", 0xff4b69ff},
    {Rarity::Rare, "rare", "Rare", 0xff8847ff},
    {Rarity::Epic, "epic", "Epic", 0xffd32ce6},
    {Rarity::Legendary, "legendary", "Legendary", 0xffeb4b4b},
    {Rarity::Exotic, "exotic", "Exotic", 0xffe4ae39},
}};

const TierInfo &tier(Rarity rarity)
{
    return kTiers[static_cast<std::size_t>(rarity)];
}

struct ItemText
{
    const char *name;
    const char *description;
};

// What each item is called and how it is described; bubble skins carry their
// own (BubbleSkins::catalog()). Every shared id needs an entry (tst_cosmetics).
const QHash<QString, ItemText> &itemTexts()
{
    static const QHash<QString, ItemText> texts = {
        // Avatar frames: drawn around the display picture, never over its middle.
        {QStringLiteral("frame.aero"), {"Aero Glass", "A tinted glass bezel with a gloss horizon and light streaks."}},
        {QStringLiteral("frame.gilded"), {"Gilded Filigree", "Polished gold with beading, scrollwork corners and a ruby."}},
        {QStringLiteral("frame.neon"), {"Neon Tubes", "Two bent glass tubes, hot pink and ice cyan, with bloom."}},
        {QStringLiteral("frame.pixel"), {"8-Bit Hero", "A chunky pixel-art bevel with rivets and an extra life."}},
        {QStringLiteral("frame.frost"), {"Frostbite", "Faceted ice, a snow cap, frost ferns and a snowflake."}},
        {QStringLiteral("frame.inferno"), {"Inferno", "Cracked lava rock wreathed in flickering flames and embers."}},
        {QStringLiteral("frame.orbit"), {"Stardust Orbit", "A deep-space band with two comets chasing round the picture."}},

        // Presence beads: the state colour always reads; the material changes.
        {QStringLiteral("bead.gem"), {"Brilliant Cut", "A faceted gemstone: emerald, citrine, ruby or smoky quartz."}},
        {QStringLiteral("bead.heart"), {"Candy Heart", "A glossy hard-candy heart."}},
        {QStringLiteral("bead.star"), {"Lucky Star", "A bevelled five-point star with a glint."}},
        {QStringLiteral("bead.orb"), {"Plasma Orb", "A glass sphere with a glowing core and halo."}},
        {QStringLiteral("bead.planet"), {"Ringed Planet", "A banded gas giant with a tilted ring."}},
        {QStringLiteral("bead.flame"), {"Spirit Flame", "A tiny will-o'-the-wisp flame."}},

        // Name flair: the name keeps its font, size and elision; only the ink changes.
        {QStringLiteral("flair.aero"), {"Aero Glow", "The Vista caption glow: a soft halo that lifts the name."}},
        {QStringLiteral("flair.chrome"), {"Y2K Chrome", "Mirror chrome with a hard horizon line and an outline."}},
        {QStringLiteral("flair.gold"), {"Gold Leaf", "Embossed gold leaf with a bright top edge."}},
        {QStringLiteral("flair.holo"), {"Holo Foil", "Iridescent foil that shifts through the spectrum, with glints."}},
        {QStringLiteral("flair.neon"), {"Neon Sign", "A glowing tube-lit name with a white-hot core."}},
        {QStringLiteral("flair.ember"), {"Molten", "White-hot to deep red, with a heat glow and sparks."}},

        // Profile scenes: a backdrop behind a header, as Messenger had.
        {QStringLiteral("scene.aurora"), {"Northern Lights", "Aurora curtains over a pine ridge under the stars."}},
        {QStringLiteral("scene.meadow"), {"Spring Meadow", "Rolling green hills, a glossy sky and drifting clouds."}},
        {QStringLiteral("scene.aqua"), {"Aqua Bubbles", "Aero light ribbons and glassy bubbles."}},
        {QStringLiteral("scene.synthwave"), {"Outrun Sunset", "A striped sun sinking behind a neon grid."}},
        {QStringLiteral("scene.sakura"), {"Sakura Breeze", "A blossoming branch shedding petals."}},
    };
    return texts;
}

QList<CosmeticInfo> buildCatalogue()
{
    QHash<QString, BubbleSkinInfo> skins;
    for (const BubbleSkinInfo &skin : BubbleSkins::catalog())
        skins.insert(skin.id, skin);
    QList<CosmeticInfo> items;
    for (const CosmeticRules::Item &rule : CosmeticRules::items()) {
        CosmeticInfo info;
        info.id = QString::fromLatin1(rule.id);
        info.category = QString::fromLatin1(rule.slot);
        info.animated = rule.animated;
        info.rarity = rule.rarity;
        if (const auto skin = skins.constFind(info.id); skin != skins.cend()) {
            info.name = skin->name;
            info.description = skin->description;
        } else if (const auto text = itemTexts().constFind(info.id); text != itemTexts().cend()) {
            info.name = QString::fromUtf8(text->name);
            info.description = QString::fromUtf8(text->description);
        } else {
            Q_ASSERT_X(false, "CosmeticCatalog", "an item has no name");
            info.name = info.id;
        }
        items.append(info);
    }
    return items;
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

QString CosmeticCatalog::categoryName(const QString &category)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("bubble"), QStringLiteral("chat bubble")},
        {QStringLiteral("frame"), QStringLiteral("avatar frame")},
        {QStringLiteral("bead"), QStringLiteral("presence bead")},
        {QStringLiteral("flair"), QStringLiteral("name flair")},
        {QStringLiteral("scene"), QStringLiteral("profile scene")},
    };
    return names.value(category);
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
    return CosmeticRules::weight(rarity);
}

const CosmeticInfo &CosmeticCatalog::draw(quint32 tierRoll, quint32 itemRoll)
{
    // The shared rules draw, so a local draw names what the relay's would.
    const CosmeticInfo *info = find(QString::fromLatin1(CosmeticRules::draw(tierRoll, itemRoll).id));
    return info ? *info : all().first();
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

QString CosmeticsCatalogObject::categoryName(const QString &category) const
{
    return CosmeticCatalog::categoryName(category);
}

QVariantList CosmeticsCatalogObject::tiers() const
{
    QVariantList list;
    for (const TierInfo &entry : kTiers) {
        list.append(QVariantMap{{QStringLiteral("id"), QString::fromLatin1(entry.id)},
                                {QStringLiteral("name"), QString::fromLatin1(entry.name)},
                                {QStringLiteral("color"), QColor::fromRgba(entry.color)},
                                {QStringLiteral("weight"), CosmeticRules::weight(entry.rarity)},
                                {QStringLiteral("percent"),
                                 100.0 * CosmeticRules::weight(entry.rarity) / CosmeticCatalog::totalWeight}});
    }
    return list;
}

} // namespace OpenChat
