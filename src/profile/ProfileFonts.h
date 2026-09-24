#pragma once

#include "domain/ProfilePage.h"

#include <QString>
#include <QStringList>

#include <optional>

// The bundled profile faces (SPEC §11): which family draws each face in each
// role, whether it may ask for bold, and the size factors that make every
// face read at the same apparent size. The files live in assets/fonts/ and are
// registered lazily, the first time a page opens, never at start-up. App
// chrome never uses them.
namespace OpenChat::ProfileFonts {

enum class Role { Body, Label, Heading, Name };

// Idempotent. A no-op until a QGuiApplication exists (GUI-less tests); GUI
// thread only.
void ensureRegistered();
[[nodiscard]] bool isRegistered();
// Every family the bundled files register, in a stable order.
[[nodiscard]] QStringList bundledFamilies();

// "" for InterfaceFont (the caller uses Theme.uiFont). Rounded body is
// "Fredoka Medium" and its labels, headings and name "Fredoka SemiBold"; Serif
// is "Playfair Display Regular" / "… Bold" the same way; Future headings are
// "Orbitron SemiBold" and names "Orbitron ExtraBold".
[[nodiscard]] QString family(Profile::Font font, Role role);
// True only where the face has a real bold: the interface font and Courier
// Prime, for labels, headings and names. The others are single-weight cuts
// that must never be synthetically emboldened.
[[nodiscard]] bool useBold(Profile::Font font, Role role);
// Name / heading factors of SPEC §11; body (and label) factors Rounded 1.08,
// Serif 1.04; 1.0 elsewhere. Pixel is 1.0: its sizes come from the grid.
[[nodiscard]] qreal sizeFactor(Profile::Font font, Role role);
// 8 for Pixel (its sizes are multiples of 8 px), else 0.
[[nodiscard]] int pixelGrid(Profile::Font font);
// The name's base pixel size: 28 / 34 / 42 × the face's name factor,
// rounded; Pixel is 16 / 24 / 32.
[[nodiscard]] int namePixelSize(Profile::Font font, Profile::NameSize size);
// The name's line box as a multiple of its size (Pacifico's tall swashes,
// Pixel's square cells), and where its baseline sits relative to the box's
// middle, as a fraction of the size (SPEC mockups' NameArt).
[[nodiscard]] qreal nameLineFactor(Profile::Font font);
[[nodiscard]] qreal nameBaselineShift(Profile::Font font);

// The face a family name belongs to (painted items are handed family names);
// nullopt for the interface font and anything unknown.
[[nodiscard]] std::optional<Profile::Font> fontForFamily(const QString &family);
// Theme.uiFont's rule: Segoe UI where installed, else the application font.
[[nodiscard]] QString interfaceFamily();

} // namespace OpenChat::ProfileFonts
