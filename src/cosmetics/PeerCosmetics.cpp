#include "cosmetics/PeerCosmetics.h"

#include "domain/CosmeticRules.h"

namespace OpenChat {

PeerCosmetics::PeerCosmetics(QObject *parent) : QObject(parent) {}

PeerCosmetics *PeerCosmetics::instance()
{
    static PeerCosmetics *peers = new PeerCosmetics;
    return peers;
}

QString PeerCosmetics::item(const QString &account, const QString &slot) const
{
    const QString id = m_loadouts.value(account).value(slot);
    return CosmeticRules::fits(id, slot) ? id : QString();
}

void PeerCosmetics::setLoadouts(const QHash<QString, Loadout> &loadouts)
{
    QHash<QString, Loadout> kept;
    for (auto it = loadouts.cbegin(); it != loadouts.cend(); ++it) {
        if (!it.value().isEmpty())
            kept.insert(it.key(), it.value());
    }
    if (kept == m_loadouts)
        return;
    m_loadouts = std::move(kept);
    bump();
}

void PeerCosmetics::setLoadout(const QString &account, const Loadout &loadout)
{
    if (m_loadouts.value(account) == loadout)
        return;
    if (loadout.isEmpty())
        m_loadouts.remove(account);
    else
        m_loadouts.insert(account, loadout);
    bump();
}

void PeerCosmetics::clear()
{
    if (m_loadouts.isEmpty())
        return;
    m_loadouts.clear();
    bump();
}

void PeerCosmetics::bump()
{
    ++m_revision;
    emit changed();
}

} // namespace OpenChat
