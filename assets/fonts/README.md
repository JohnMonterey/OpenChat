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
| Future | `OpenChat Future SemiBold`, `OpenChat Future ExtraBold` | `orbitron/OpenChatFuture-SemiBold.ttf`, `orbitron/OpenChatFuture-ExtraBold.ttf` | renamed static cuts of Orbitron (`Orbitron[wght].ttf` 2.001) at wght 600 / 800 | OFL 1.1 (`orbitron/OFL.txt`) |
| Serif | `OpenChat Serif Regular`, `OpenChat Serif Bold` | `playfairdisplay/OpenChatSerif-Regular.ttf`, `playfairdisplay/OpenChatSerif-Bold.ttf` | renamed static cuts of Playfair Display (`PlayfairDisplay[wght].ttf` 1.203) at wght 400 / 700 | OFL 1.1 (`playfairdisplay/OFL.txt`) |
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
400, no STAT table) and keeps the copyright, trademark, designer, licence and
version records. It also copies each family's `OFL.txt` beside its files.

## Modified Versions: OpenChat Future and OpenChat Serif

A static cut is a Modified Version of the font under the SIL Open Font
License 1.1, and the licence (condition 3) forbids a Modified Version from
using a Reserved Font Name. Orbitron reserves "Orbitron" and Playfair Display
reserves "Playfair Display" (see the first line of each `OFL.txt`), so our
cuts are distributed under the SIL OFL 1.1 with new names:

- **OpenChat Future** (`OpenChat Future SemiBold`, `OpenChat Future
  ExtraBold`) is a Modified Version of **Orbitron** by Matt McInerney
  (The League of Moveable Type), Copyright 2018 The Orbitron Project Authors,
  <https://github.com/theleagueof/orbitron>.
- **OpenChat Serif** (`OpenChat Serif Regular`, `OpenChat Serif Bold`) is a
  Modified Version of **Playfair Display** by Claus Eggers Sørensen,
  Copyright 2017 The Playfair Display Project Authors,
  <https://github.com/clauseggers/Playfair-Display>.

The outlines, metrics and OpenType features are the originals' at those
weights; only the naming records differ. Every family, full, unique,
PostScript, typographic and other naming record says the new name; the
copyright and licence records (and Playfair's trademark notice) are kept
verbatim, as the licence requires, and `make_static.py` refuses to write a cut
whose other records still carry the reserved name. The folders keep their
upstream names and each keeps its unmodified `OFL.txt`. The profile editor
labels these faces "Future" and "Serif"; those labels are OpenChat's own.

Fredoka reserves no font name, so its cuts keep it. Press Start 2P and
UnifrakturMaguntia reserve theirs but are shipped unmodified, which the
licence allows.
