# Profile page fonts

The faces a profile page owner can choose for their headings, body text and
name (design spec §11). App chrome never uses them: it stays in the interface
font. They are compiled into the `openchat_profile` library as the Qt resource
`openchat_profile_fonts` (prefix `/openchat/profile-fonts`) and registered
lazily, by `ProfileFonts::ensureRegistered()`, the first time a profile page
opens, so starting OpenChat never loads them.

| Face (`Profile::Font`) | Family as registered | Files | Source | Licence |
|---|---|---|---|---|
| Rounded | `Fredoka Medium`, `Fredoka SemiBold` | `fredoka/FredokaMedium.ttf`, `fredoka/FredokaSemiBold.ttf` | static cuts of `Fredoka[wdth,wght].ttf` 2.001 at wght 500 / 600, wdth 100 | OFL 1.1 (`fredoka/OFL.txt`) |
| Script | `Pacifico` | `pacifico/Pacifico-Regular.ttf` | Pacifico 3.001, unmodified | OFL 1.1 |
| Typewriter | `Courier Prime` (Regular, Bold) | `courierprime/CourierPrime-Regular.ttf`, `courierprime/CourierPrime-Bold.ttf` | Courier Prime 3.018, unmodified | OFL 1.1 |
| Pixel (names only) | `Press Start 2P` | `pressstart2p/PressStart2P-Regular.ttf` | Press Start 2P 3.000, unmodified | OFL 1.1 |
| Gothic | `UnifrakturMaguntia` | `unifrakturmaguntia/UnifrakturMaguntia-Book.ttf` | UnifrakturMaguntia 2010-11-24, unmodified | OFL 1.1 |
| Future | `Orbitron SemiBold`, `Orbitron ExtraBold` | `orbitron/OrbitronSemiBold.ttf`, `orbitron/OrbitronExtraBold.ttf` | static cuts of `Orbitron[wght].ttf` 2.001 at wght 600 / 800 | OFL 1.1 |
| Serif | `Playfair Display Regular`, `Playfair Display Bold` | `playfairdisplay/PlayfairDisplayRegular.ttf`, `playfairdisplay/PlayfairDisplayBold.ttf` | static cuts of `PlayfairDisplay[wght].ttf` 1.203 at wght 400 / 700 | OFL 1.1 |
| Marker | `Permanent Marker` | `permanentmarker/PermanentMarker-Regular.ttf` | Permanent Marker 1.001, unmodified | Apache 2.0 (`permanentmarker/LICENSE.txt`) |

All sources are the files published in the google/fonts repository
(`ofl/<family>/`, `apache/permanentmarker/`). About 1.3 MB in total; glyphs a
face lacks (★ ♥ ♫ ✿ in most of them) fall back to the system font per glyph.

## Static instances

Fredoka, Orbitron and Playfair Display are published only as variable fonts.
The renderer picks every face by family name alone, identically on every
platform (no variable-axis support needed, no synthetic bold), so the two
weights used of each are shipped as static fonts whose family name carries the
weight. They were cut with fontTools 4.65 by

    python3 tools/fonts/make_static.py --source <dir with fredoka/ orbitron/ playfairdisplay/>

which pins every axis (`fontTools.varLib.instancer`), rewrites only the naming
records (family = e.g. "Fredoka SemiBold", subfamily "Regular", weight class
400, no STAT table) and keeps the copyright, licence and version records. It
also copies each family's `OFL.txt` beside its files.

Orbitron and Playfair Display declare Reserved Font Names in their OFL
headers; see the licence texts before renaming or redistributing the cuts in
another form.
