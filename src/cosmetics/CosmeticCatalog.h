#pragma once

#include <QColor>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace OpenChat {

// How rare a collectible is, lowest first. The daily case rolls a tier by its
// weight, then one item of that tier uniformly, so an item's odds are its
// tier's share divided by the tier's size.
enum class Rarity { Common, Rare, Epic, Legendary, Exotic };

// Every collectible cosmetic the client can draw, by stable id. The ids are
// what settings (and later the inventory and the daily case) store, so they
// never change once shipped; names, descriptions and rarity are presentation.
struct CosmeticInfo
{
    QString id;
    QString category; // "bubble", "frame", "bead", "flair" or "scene"
    QString name;
    QString description;
    bool animated = false;
    Rarity rarity = Rarity::Common;
};

class CosmeticCatalog
{
  public:
    // Tier weights are out of this total (parts per thousand).
    static constexpr int totalWeight = 1000;

    static const QList<CosmeticInfo> &all();
    static QList<CosmeticInfo> inCategory(const QString &category);
    static QList<CosmeticInfo> ofRarity(Rarity rarity);
    static const CosmeticInfo *find(const QString &id);
    // True when `id` names an item of `category`; empty and unknown ids are not.
    static bool isKnown(const QString &id, const QString &category);

    static QList<Rarity> rarities();
    static QString rarityId(Rarity rarity);   // "common" … "exotic"
    static QString rarityName(Rarity rarity); // "Common" … "Exotic"
    static QColor rarityColor(Rarity rarity);
    static int rarityWeight(Rarity rarity);

    // One draw: `tierRoll` modulo totalWeight picks the tier by weight, then
    // `itemRoll` picks within it. Deterministic, so the same rolls always
    // name the same item; the caller supplies the randomness.
    static const CosmeticInfo &draw(quint32 tierRoll, quint32 itemRoll);
};

// The catalogue as a QML singleton, for the gallery and the future inventory.
class CosmeticsCatalogObject : public QObject
{
    Q_OBJECT
  public:
    explicit CosmeticsCatalogObject(QObject *parent = nullptr);
    // [{id, category, name, description, animated, rarity, rarityName,
    //   rarityColor}] in display order.
    Q_INVOKABLE QVariantList items(const QString &category) const;
    Q_INVOKABLE QVariantMap item(const QString &id) const;
    Q_INVOKABLE QString displayName(const QString &id) const;
    // [{id, name, color, weight, percent}] from Common to Exotic.
    Q_INVOKABLE QVariantList tiers() const;
};

// One catalogue entry as the map QML reads; empty for an unknown id.
QVariantMap cosmeticToVariant(const CosmeticInfo *info);

} // namespace OpenChat
