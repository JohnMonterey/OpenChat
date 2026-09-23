#include "cosmetics/CosmeticCatalog.h"

#include <QVariantMap>

namespace OpenChat {

const QList<CosmeticInfo> &CosmeticCatalog::all()
{
    static const QList<CosmeticInfo> items = {
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
    };
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

CosmeticsCatalogObject::CosmeticsCatalogObject(QObject *parent) : QObject(parent) {}

QVariantList CosmeticsCatalogObject::items(const QString &category) const
{
    QVariantList list;
    for (const CosmeticInfo &info : CosmeticCatalog::inCategory(category)) {
        list.append(QVariantMap{{QStringLiteral("id"), info.id},
                                {QStringLiteral("name"), info.name},
                                {QStringLiteral("description"), info.description},
                                {QStringLiteral("animated"), info.animated}});
    }
    return list;
}

QString CosmeticsCatalogObject::displayName(const QString &id) const
{
    const CosmeticInfo *info = CosmeticCatalog::find(id);
    return info ? info->name : QString();
}

} // namespace OpenChat
