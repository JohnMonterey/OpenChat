#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace OpenChat {

// How rare a collectible is, lowest first. A case rolls a tier by its weight,
// then one item of that tier uniformly, so an item's odds are its tier's share
// divided by the tier's size.
enum class Rarity { Common, Rare, Epic, Legendary, Exotic };

// The collectibles the client and the relay agree on: each item's stable id,
// the slot it is worn in, its tier and whether it moves. The relay draws case
// rewards and checks what may be equipped from this table, so both sides
// always name the same things with the same odds. Names, descriptions and
// colours are presentation and live in the client (cosmetics/CosmeticCatalog).
// Ids never change once shipped; a new item is appended to its slot's run.
namespace CosmeticRules {

struct Item
{
    const char *id;
    const char *slot; // "bubble", "frame", "bead", "flair" or "scene"
    Rarity rarity;
    bool animated;
};

// Tier weights are out of this total (parts per thousand).
inline constexpr int totalWeight = 1000;

// Every item, in display order.
const QList<Item> &items();
const Item *find(QStringView id);
// True when `id` names an item worn in `slot`; empty and unknown ids are not.
bool fits(QStringView id, QStringView slot);
// The slots, in the order Settings lists them.
QStringList slotNames();

QList<Rarity> rarities();
int weight(Rarity rarity);

// One draw: `tierRoll` modulo totalWeight picks the tier by weight, then
// `itemRoll` picks within it. Deterministic, so the same rolls always name
// the same item; the caller supplies the randomness.
const Item &draw(quint32 tierRoll, quint32 itemRoll);

} // namespace CosmeticRules
} // namespace OpenChat
