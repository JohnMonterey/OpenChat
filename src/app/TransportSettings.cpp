#include "app/TransportSettings.h"

#include <QSettings>

namespace OpenChat {

namespace {
TransportSettings *s_instance = nullptr;
constexpr auto settingsKey = "Transport/mode";
} // namespace

TransportSettings::TransportSettings(QObject *parent)
    : QObject(parent)
{
    if (s_instance == nullptr)
        s_instance = this;

    QSettings settings;
    const QString saved = settings.value(QString::fromLatin1(settingsKey), QStringLiteral("auto")).toString();
    setMode(saved);
}

TransportSettings::~TransportSettings()
{
    if (s_instance == this)
        s_instance = nullptr;
}

TransportSettings *TransportSettings::instance()
{
    return s_instance;
}

QString TransportSettings::mode() const
{
    switch (m_mode) {
    case TransportMode::Auto:
        return QStringLiteral("auto");
    case TransportMode::Udp:
        return QStringLiteral("udp");
    case TransportMode::Tcp:
        return QStringLiteral("tcp");
    }
    return QStringLiteral("auto");
}

void TransportSettings::setMode(const QString &modeStr)
{
    TransportMode newMode = TransportMode::Auto;
    if (modeStr.compare(QLatin1String("udp"), Qt::CaseInsensitive) == 0)
        newMode = TransportMode::Udp;
    else if (modeStr.compare(QLatin1String("tcp"), Qt::CaseInsensitive) == 0)
        newMode = TransportMode::Tcp;
    else
        newMode = TransportMode::Auto;

    setTransportMode(newMode);
}

void TransportSettings::setTransportMode(TransportMode mode)
{
    if (m_mode == mode)
        return;

    m_mode = mode;
    QSettings settings;
    settings.setValue(QString::fromLatin1(settingsKey), this->mode());
    emit modeChanged();
}

} // namespace OpenChat
