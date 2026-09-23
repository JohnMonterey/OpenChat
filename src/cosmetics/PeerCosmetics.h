#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace OpenChat {

// What other people wear, as the relay holds it (each account's loadout of
// catalogue ids, slot -> id), for every surface that shows someone else: their
// row, the conversation header, their message bubbles and their call tile.
// ChatController fills it from the relay; QML reads it through the
// `PeerCosmetics` singleton, binding to `revision` so a new loadout redraws:
//
//     frameId: PeerCosmetics.revision >= 0 ? PeerCosmetics.item(contactId, "frame") : ""
//
// Only ids this build knows, in the slot they belong to, are ever handed out.
class PeerCosmetics final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    using Loadout = QHash<QString, QString>;

    // The one the app shows; QML's singleton is this same object.
    static PeerCosmetics *instance();

    [[nodiscard]] int revision() const { return m_revision; }
    // The item `account` (hex) wears in `slot`, or empty.
    Q_INVOKABLE QString item(const QString &account, const QString &slot) const;

    // Replaces everything known with `loadouts` (account hex -> loadout).
    void setLoadouts(const QHash<QString, Loadout> &loadouts);
    // One account's loadout (empty: it wears nothing).
    void setLoadout(const QString &account, const Loadout &loadout);
    void clear();

signals:
    void changed();

private:
    explicit PeerCosmetics(QObject *parent = nullptr);
    void bump();

    QHash<QString, Loadout> m_loadouts;
    int m_revision = 0;
};

} // namespace OpenChat
