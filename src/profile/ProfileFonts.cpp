#include "profile/ProfileFonts.h"

#include "diagnostics/Logging.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <array>
#include <cmath>

namespace OpenChat::ProfileFonts {

namespace {

using Profile::Font;

const QString resourceRoot = QStringLiteral(":/openchat/profile-fonts/");

constexpr std::array<const char *, 12> fontFiles{
    "fredoka/FredokaMedium.ttf",
    "fredoka/FredokaSemiBold.ttf",
    "pacifico/Pacifico-Regular.ttf",
    "courierprime/CourierPrime-Regular.ttf",
    "courierprime/CourierPrime-Bold.ttf",
    "pressstart2p/PressStart2P-Regular.ttf",
    "unifrakturmaguntia/UnifrakturMaguntia-Book.ttf",
    "orbitron/OrbitronSemiBold.ttf",
    "orbitron/OrbitronExtraBold.ttf",
    "playfairdisplay/PlayfairDisplayRegular.ttf",
    "playfairdisplay/PlayfairDisplayBold.ttf",
    "permanentmarker/PermanentMarker-Regular.ttf",
};

QMutex &registrationMutex()
{
    static QMutex mutex;
    return mutex;
}

bool &registeredFlag()
{
    static bool registered = false;
    return registered;
}

} // namespace

void ensureRegistered()
{
    QMutexLocker locker(&registrationMutex());
    if (registeredFlag())
        return;
    // The font database needs a GUI application; GUI-less processes (the
    // controller tests, tst_e2e) never draw a page.
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return;
    for (const char *file : fontFiles) {
        const QString path = resourceRoot + QLatin1String(file);
        if (QFontDatabase::addApplicationFont(path) < 0)
            qCWarning(mediaLog) << "profile font did not register:" << path;
    }
    registeredFlag() = true;
}

bool isRegistered()
{
    QMutexLocker locker(&registrationMutex());
    return registeredFlag();
}

QStringList bundledFamilies()
{
    return {QStringLiteral("Fredoka Medium"),
            QStringLiteral("Fredoka SemiBold"),
            QStringLiteral("Pacifico"),
            QStringLiteral("Courier Prime"),
            QStringLiteral("Press Start 2P"),
            QStringLiteral("UnifrakturMaguntia"),
            QStringLiteral("Orbitron SemiBold"),
            QStringLiteral("Orbitron ExtraBold"),
            QStringLiteral("Playfair Display Regular"),
            QStringLiteral("Playfair Display Bold"),
            QStringLiteral("Permanent Marker")};
}

QString family(Font font, Role role)
{
    const bool body = role == Role::Body;
    switch (font) {
    case Font::InterfaceFont:
        return {};
    case Font::RoundedFont:
        return body ? QStringLiteral("Fredoka Medium") : QStringLiteral("Fredoka SemiBold");
    case Font::ScriptFont:
        return QStringLiteral("Pacifico");
    case Font::TypewriterFont:
        return QStringLiteral("Courier Prime");
    case Font::PixelFont:
        return QStringLiteral("Press Start 2P");
    case Font::GothicFont:
        return QStringLiteral("UnifrakturMaguntia");
    case Font::FutureFont:
        return role == Role::Name ? QStringLiteral("Orbitron ExtraBold") : QStringLiteral("Orbitron SemiBold");
    case Font::SerifFont:
        return body ? QStringLiteral("Playfair Display Regular") : QStringLiteral("Playfair Display Bold");
    case Font::MarkerFont:
        return QStringLiteral("Permanent Marker");
    }
    return {};
}

bool useBold(Font font, Role role)
{
    if (role == Role::Body)
        return false;
    return font == Font::InterfaceFont || font == Font::TypewriterFont;
}

qreal sizeFactor(Font font, Role role)
{
    switch (role) {
    case Role::Body:
    case Role::Label:
        return font == Font::RoundedFont ? 1.08 : font == Font::SerifFont ? 1.04 : 1.0;
    case Role::Name:
        switch (font) {
        case Font::ScriptFont:
        case Font::MarkerFont:
            return 0.95;
        case Font::GothicFont:
            return 1.18;
        case Font::FutureFont:
            return 0.90;
        default:
            return 1.0;
        }
    case Role::Heading:
        switch (font) {
        case Font::RoundedFont:
            return 1.04;
        case Font::ScriptFont:
            return 0.92;
        case Font::GothicFont:
            return 1.18;
        case Font::FutureFont:
            return 0.86;
        case Font::MarkerFont:
            return 0.96;
        default:
            return 1.0;
        }
    }
    return 1.0;
}

int pixelGrid(Font font)
{
    return font == Font::PixelFont ? 8 : 0;
}

int namePixelSize(Font font, Profile::NameSize size)
{
    const int index = std::clamp(int(size), 0, 2);
    if (font == Font::PixelFont)
        return (index + 2) * pixelGrid(font); // 16 / 24 / 32
    static constexpr int bases[] = {28, 34, 42};
    return int(std::lround(bases[index] * sizeFactor(font, Role::Name)));
}

qreal nameLineFactor(Font font)
{
    switch (font) {
    case Font::ScriptFont:
        return 1.5;
    case Font::GothicFont:
        return 1.18;
    case Font::PixelFont:
        return 1.1;
    default:
        return 1.26;
    }
}

qreal nameBaselineShift(Font font)
{
    return font == Font::ScriptFont ? -0.07 : font == Font::GothicFont ? 0.03 : 0.0;
}

std::optional<Font> fontForFamily(const QString &name)
{
    static constexpr Font faces[] = {Font::RoundedFont, Font::ScriptFont, Font::TypewriterFont, Font::PixelFont,
                                     Font::GothicFont,  Font::FutureFont, Font::SerifFont,      Font::MarkerFont};
    for (const Font font : faces) {
        for (const Role role : {Role::Body, Role::Name}) {
            if (family(font, role).compare(name, Qt::CaseInsensitive) == 0)
                return font;
        }
    }
    return std::nullopt;
}

QString interfaceFamily()
{
    static const QString segoe = QStringLiteral("Segoe UI");
    if (QFontDatabase::hasFamily(segoe))
        return segoe;
    return QGuiApplication::font().family();
}

} // namespace OpenChat::ProfileFonts
