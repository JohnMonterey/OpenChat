#pragma once

#include <QAbstractListModel>
#include <QImage>
#include <QString>
#include <QVector>

namespace OpenChat {

// One other member of a group call, as the call screen shows them: who they
// are, what they are doing (ringing, in the call, declined, left...), whether
// they are talking, and their camera picture when they share it.
struct CallParticipantRow final {
    QString deviceId; // hex, the key updates are matched on
    QString name;
    QString avatarKey;
    QString stateText; // empty while in the call; otherwise "Ringing…", "Left", ...
    bool joined = false;
    bool ringing = false;
    bool speaking = false;
    double level = 0.0;
    QImage videoFrame;
};

class CallParticipantModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        DeviceIdRole = Qt::UserRole + 1,
        NameRole,
        AvatarKeyRole,
        StateTextRole,
        JoinedRole,
        RingingRole,
        SpeakingRole,
        LevelRole,
        VideoFrameRole,
        CameraEnabledRole,
        VideoAspectRole,
    };
    Q_ENUM(Role)

    explicit CallParticipantModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const;

    // Replaces the roster, keeping each row's camera picture when the same
    // device is still listed, so a roster refresh never blanks a live camera.
    void setParticipants(QVector<CallParticipantRow> rows);
    void setVideoFrame(const QString &deviceId, const QImage &frame);
    [[nodiscard]] QVector<CallParticipantRow> participants() const { return m_rows; }

signals:
    void countChanged();

private:
    QVector<CallParticipantRow> m_rows;
};

} // namespace OpenChat
