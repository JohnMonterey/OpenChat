#include "domain/CosmeticRules.h"

#include <array>

namespace OpenChat::CosmeticRules {

namespace {

struct Tier
{
    Rarity rarity;
    int weight; // out of totalWeight
};

// The ladder's odds: gentler than a paid crate's because cases are free,
// something Epic or better about every seven cases.
constexpr std::array<Tier, 5> kTiers = {{
    {Rarity::Common, 600},
    {Rarity::Rare, 250},
    {Rarity::Epic, 100},
    {Rarity::Legendary, 40},
    {Rarity::Exotic, 10},
}};

constexpr int weightSum()
{
    int sum = 0;
    for (const Tier &tier : kTiers)
        sum += tier.weight;
    return sum;
}
static_assert(weightSum() == totalWeight, "tier weights must cover every roll");

// Every item with its tier, in one place so the ladder can be balanced at a
// glance. Small or plain items are Common; the rarer an item, the more of the
// screen it takes, the more it does, or both. Animated items sit at the top.
// The order is the display order, and within a tier it is the draw order.
constexpr Item kItems[] = {
    // Chat-bubble skins.
    {"bubble.aero", "bubble", Rarity::Common, false},
    {"bubble.marble", "bubble", Rarity::Epic, false},
    {"bubble.nebula", "bubble", Rarity::Legendary, false},
    {"bubble.magma", "bubble", Rarity::Exotic, false},
    {"bubble.holo", "bubble", Rarity::Legendary, false},
    // Avatar frames.
    {"frame.aero", "frame", Rarity::Common, false},
    {"frame.gilded", "frame", Rarity::Epic, false},
    {"frame.neon", "frame", Rarity::Rare, false},
    {"frame.pixel", "frame", Rarity::Common, false},
    {"frame.frost", "frame", Rarity::Epic, false},
    {"frame.inferno", "frame", Rarity::Exotic, true},
    {"frame.orbit", "frame", Rarity::Legendary, true},
    // Presence beads.
    {"bead.gem", "bead", Rarity::Rare, false},
    {"bead.heart", "bead", Rarity::Common, false},
    {"bead.star", "bead", Rarity::Common, false},
    {"bead.orb", "bead", Rarity::Common, false},
    {"bead.planet", "bead", Rarity::Rare, false},
    {"bead.flame", "bead", Rarity::Rare, false},
    // Name flair.
    {"flair.aero", "flair", Rarity::Common, false},
    {"flair.chrome", "flair", Rarity::Epic, false},
    {"flair.gold", "flair", Rarity::Common, false},
    {"flair.holo", "flair", Rarity::Epic, false},
    {"flair.neon", "flair", Rarity::Rare, false},
    {"flair.ember", "flair", Rarity::Epic, false},
    // Profile scenes.
    {"scene.aurora", "scene", Rarity::Legendary, false},
    {"scene.meadow", "scene", Rarity::Rare, false},
    {"scene.aqua", "scene", Rarity::Rare, false},
    {"scene.synthwave", "scene", Rarity::Legendary, false},
    {"scene.sakura", "scene", Rarity::Epic, false},
};

// Each tier's members, in display order, for draw().
const std::array<QList<const Item *>, 5> &tierMembers()
{
    static const auto members = [] {
        std::array<QList<const Item *>, 5> lists;
        for (const Item &item : items())
            lists[static_cast<std::size_t>(item.rarity)].append(&item);
        return lists;
    }();
    return members;
}

} // namespace

const QList<Item> &items()
{
    static const QList<Item> list(std::begin(kItems), std::end(kItems));
    return list;
}

const Item *find(QStringView id)
{
    for (const Item &item : items()) {
        if (id == QLatin1StringView(item.id))
            return &item;
    }
    return nullptr;
}

bool fits(QStringView id, QStringView slot)
{
    const Item *item = find(id);
    return item && slot == QLatin1StringView(item->slot);
}

QStringList slotNames()
{
    return {QStringLiteral("frame"), QStringLiteral("flair"), QStringLiteral("bead"),
            QStringLiteral("scene"), QStringLiteral("bubble")};
}

QList<Rarity> rarities()
{
    QList<Rarity> list;
    for (const Tier &tier : kTiers)
        list.append(tier.rarity);
    return list;
}

int weight(Rarity rarity)
{
    return kTiers[static_cast<std::size_t>(rarity)].weight;
}

const Item &draw(quint32 tierRoll, quint32 itemRoll)
{
    int roll = static_cast<int>(tierRoll % static_cast<quint32>(totalWeight));
    for (const Tier &tier : kTiers) {
        if (roll < tier.weight) {
            const QList<const Item *> &members = tierMembers()[static_cast<std::size_t>(tier.rarity)];
            if (members.isEmpty())
                break;
            return *members[static_cast<qsizetype>(itemRoll % static_cast<quint32>(members.size()))];
        }
        roll -= tier.weight;
    }
    return items().first();
}

} // namespace OpenChat::CosmeticRules
