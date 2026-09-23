#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace OpenChat {

// Every collectible profile cosmetic the client can draw, by stable id. The
// ids are what settings (and later the inventory and the daily case) store, so
// they never change once shipped; names and descriptions are presentation.
struct CosmeticInfo
{
    QString id;
    QString category; // "frame", "bead", "flair" or "scene"
    QString name;
    QString description;
    bool animated = false;
};

class CosmeticCatalog
{
  public:
    static const QList<CosmeticInfo> &all();
    static QList<CosmeticInfo> inCategory(const QString &category);
    static const CosmeticInfo *find(const QString &id);
    // True when `id` names an item of `category`; empty and unknown ids are not.
    static bool isKnown(const QString &id, const QString &category);
};

// The catalogue as a QML singleton, for the gallery and the future inventory.
class CosmeticsCatalogObject : public QObject
{
    Q_OBJECT
  public:
    explicit CosmeticsCatalogObject(QObject *parent = nullptr);
    // [{id, name, description, animated}] in display order.
    Q_INVOKABLE QVariantList items(const QString &category) const;
    Q_INVOKABLE QString displayName(const QString &id) const;
};

} // namespace OpenChat
