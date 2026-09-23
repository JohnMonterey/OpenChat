#pragma once

#include <QObject>
#include <QString>

namespace OpenChat {

enum class TransportMode {
    Auto,
    Udp,
    Tcp,
};

// Transport preference for voice calls: auto (UDP with WS fallback), udp (pure UDP),
// or tcp (relay WebSocket only). Persisted in QSettings and exposed as a QML singleton.
class TransportSettings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)

public:
    explicit TransportSettings(QObject *parent = nullptr);
    ~TransportSettings() override;

    [[nodiscard]] static TransportSettings *instance();

    [[nodiscard]] QString mode() const;
    void setMode(const QString &mode);

    [[nodiscard]] TransportMode transportMode() const { return m_mode; }
    void setTransportMode(TransportMode mode);

signals:
    void modeChanged();

private:
    TransportMode m_mode = TransportMode::Auto;
};

} // namespace OpenChat
