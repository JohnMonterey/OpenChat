#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVector>

#include <optional>

namespace OpenChat {

// One card in the composer's attachment tray: a file the user picked (or
// pasted) that is being prepared to send, is ready, or could not be prepared.
// Plain values only, like Message: the prepared bytes stay with the
// controller.
struct StagedAttachment {
    QString id;
    int kind = 4;            // 1 photo, 2 video, 3 audio, 4 file (a guess until ready)
    QString name;            // the file's name
    qint64 byteCount = 0;    // 0 until known
    qreal progress = 0.0;    // 0…1 while preparing
    bool ready = false;
    bool failed = false;
    QString error;           // why it failed ("Files up to 16 MB can be sent.")
    QString notice;          // what changed on the way ("Only the first minute will be sent.")
    QString previewKey;      // a PanelMediaLibrary key, empty when there is no picture
    qint64 durationMs = 0;   // video and audio, once known
};

// The composer's attachment tray (docs/chat-attachments.md). Rows are
// inserted and removed one by one and only the roles that moved are
// signalled, so a card's progress never rebuilds the tray.
class StagedAttachmentModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        StagedIdRole = Qt::UserRole + 1,
        KindRole,
        NameRole,
        SizeTextRole,
        ProgressRole,
        ReadyRole,
        FailedRole,
        ErrorRole,
        NoticeRole,
        PreviewKeyRole,
        DurationTextRole,
    };
    Q_ENUM(Role)

    explicit StagedAttachmentModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const { return int(m_items.size()); }

    void append(const StagedAttachment &item);
    bool remove(const QString &id);
    void clear();
    // Replaces the row with `item.id`, signalling only the roles that moved.
    // Returns whether a row was found.
    bool update(const StagedAttachment &item);
    // Progress alone, signalled only when it moved by a percent or more (or
    // reached the end), however often the importer reports.
    bool setProgress(const QString &id, qreal progress);

    [[nodiscard]] std::optional<StagedAttachment> item(const QString &id) const;
    [[nodiscard]] const QVector<StagedAttachment> &items() const noexcept { return m_items; }
    [[nodiscard]] int rowOf(const QString &id) const;

signals:
    void countChanged();

private:
    QVector<StagedAttachment> m_items;
};

} // namespace OpenChat
