#include "models/CallParticipantModel.h"

namespace OpenChat {

CallParticipantModel::CallParticipantModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int CallParticipantModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant CallParticipantModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.column() != 0 || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const CallParticipantRow &row = m_rows.at(index.row());
    switch (role) {
    case DeviceIdRole:
        return row.deviceId;
    case NameRole:
        return row.name;
    case AvatarKeyRole:
        return row.avatarKey;
    case StateTextRole:
        return row.stateText;
    case JoinedRole:
        return row.joined;
    case RingingRole:
        return row.ringing;
    case SpeakingRole:
        return row.speaking;
    case LevelRole:
        return row.level;
    case VideoFrameRole:
        return row.videoFrame;
    case CameraEnabledRole:
        return !row.videoFrame.isNull();
    case VideoAspectRole:
        return row.videoFrame.isNull() ? 4.0 / 3.0
                                       : double(row.videoFrame.width()) / row.videoFrame.height();
    default:
        return {};
    }
}

QHash<int, QByteArray> CallParticipantModel::roleNames() const
{
    return {
        {DeviceIdRole, "deviceId"},
        {NameRole, "name"},
        {AvatarKeyRole, "avatarKey"},
        {StateTextRole, "stateText"},
        {JoinedRole, "joined"},
        {RingingRole, "ringing"},
        {SpeakingRole, "speaking"},
        {LevelRole, "level"},
        {VideoFrameRole, "videoFrame"},
        {CameraEnabledRole, "cameraEnabled"},
        {VideoAspectRole, "videoAspect"},
    };
}

int CallParticipantModel::count() const
{
    return m_rows.size();
}

void CallParticipantModel::setParticipants(QVector<CallParticipantRow> rows)
{
    for (CallParticipantRow &row : rows) {
        for (const CallParticipantRow &existing : m_rows)
            if (existing.deviceId == row.deviceId && row.joined)
                row.videoFrame = existing.videoFrame;
    }
    // Same devices in the same order: update in place so the delegates keep
    // their state (and their video items) rather than being rebuilt.
    bool sameShape = rows.size() == m_rows.size();
    for (int i = 0; sameShape && i < rows.size(); ++i)
        sameShape = rows.at(i).deviceId == m_rows.at(i).deviceId;
    if (sameShape) {
        m_rows = std::move(rows);
        if (!m_rows.isEmpty())
            emit dataChanged(index(0), index(m_rows.size() - 1));
        return;
    }
    const int previousCount = m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (previousCount != m_rows.size())
        emit countChanged();
}

void CallParticipantModel::setVideoFrame(const QString &deviceId, const QImage &frame)
{
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows[row].deviceId != deviceId)
            continue;
        const bool sizeChanged = m_rows[row].videoFrame.size() != frame.size();
        m_rows[row].videoFrame = frame;
        QList<int> roles{VideoFrameRole};
        if (sizeChanged)
            roles << CameraEnabledRole << VideoAspectRole;
        emit dataChanged(index(row), index(row), roles);
        return;
    }
}

} // namespace OpenChat
