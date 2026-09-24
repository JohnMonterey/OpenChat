#!/usr/bin/env python3
"""Cut the static font instances OpenChat's profile pages ship.

Three of the bundled profile faces are published only as variable fonts
(Fredoka, Orbitron, Playfair Display). The renderer selects every face by its
family name alone, on every platform, so it ships the two weights it uses of
each as plain static fonts instead, named after the weight:

    Fredoka Medium, Fredoka SemiBold                  (Fredoka, wght 500 / 600, wdth 100)
    OpenChat Future SemiBold, OpenChat Future ExtraBold   (Orbitron, wght 600 / 800)
    OpenChat Serif Regular, OpenChat Serif Bold           (Playfair Display, wght 400 / 700)

A cut is a Modified Version under the SIL Open Font License 1.1, and OFL
condition 3 forbids a Modified Version from using a Reserved Font Name.
Orbitron reserves "Orbitron" and Playfair Display reserves "Playfair
Display", so their cuts are renamed "OpenChat Future" and "OpenChat Serif" in
every naming record (family, full, unique id, PostScript, typographic and WWS
names, and any other record that would carry the reserved name). Fredoka
reserves no name and keeps its own. The script refuses to write a renamed cut
that still names its original anywhere but the attribution records
(copyright, trademark notice, manufacturer, designer, URLs, licence).

Usage:
    python3 tools/fonts/make_static.py --source DIR [--out assets/fonts]

DIR holds the upstream files in the google/fonts layout: fredoka/,
orbitron/ and playfairdisplay/ with their variable .ttf and OFL.txt. The
output goes to OUT/<folder>/<file>.ttf (see JOBS), beside a copy of the
family's OFL.txt. The copyright, licence and version records of the source
are kept; only the naming records change. Needs fontTools (pip install
fonttools).
"""
import argparse
import os
import shutil

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

# Reserved Font Names (from each OFL.txt header) and what a cut says instead,
# longest first. Fredoka reserves none.
ORBITRON_RENAMES = (("Orbitron", "OpenChat Future"),)
PLAYFAIR_RENAMES = (
    ("Playfair Display", "OpenChat Serif"),
    ("PlayfairDisplay", "OpenChatSerif"),
    ("Playfair", "OpenChat Serif"),
)

# (folder, variable source, axis location, family, PostScript name, file,
#  reserved-name renames)
JOBS = [
    ("fredoka", "Fredoka[wdth,wght].ttf", {"wght": 500, "wdth": 100},
     "Fredoka Medium", "FredokaMedium", "FredokaMedium.ttf", ()),
    ("fredoka", "Fredoka[wdth,wght].ttf", {"wght": 600, "wdth": 100},
     "Fredoka SemiBold", "FredokaSemiBold", "FredokaSemiBold.ttf", ()),
    ("orbitron", "Orbitron[wght].ttf", {"wght": 600},
     "OpenChat Future SemiBold", "OpenChatFuture-SemiBold", "OpenChatFuture-SemiBold.ttf", ORBITRON_RENAMES),
    ("orbitron", "Orbitron[wght].ttf", {"wght": 800},
     "OpenChat Future ExtraBold", "OpenChatFuture-ExtraBold", "OpenChatFuture-ExtraBold.ttf", ORBITRON_RENAMES),
    ("playfairdisplay", "PlayfairDisplay[wght].ttf", {"wght": 400},
     "OpenChat Serif Regular", "OpenChatSerif-Regular", "OpenChatSerif-Regular.ttf", PLAYFAIR_RENAMES),
    ("playfairdisplay", "PlayfairDisplay[wght].ttf", {"wght": 700},
     "OpenChat Serif Bold", "OpenChatSerif-Bold", "OpenChatSerif-Bold.ttf", PLAYFAIR_RENAMES),
]

# Naming records rewritten for the instance: family, subfamily, unique id,
# full name, PostScript name, typographic family/subfamily, WWS names and the
# variations PostScript prefix. Only 1, 2, 3, 4 and 6 are written back.
RENAMED_IDS = (1, 2, 3, 4, 6, 16, 17, 21, 22, 25)

# Attribution records, kept verbatim even where they mention the original
# font (the copyright line names its Reserved Font Name; Playfair's trademark
# notice): copyright, version, trademark, manufacturer, designer,
# description, vendor/designer URLs, licence and licence URL.
ATTRIBUTION_IDS = frozenset((0, 5, 7, 8, 9, 10, 11, 12, 13, 14))


def rename(font, family, postscript, renames):
    name = font["name"]
    version = name.getDebugName(5) or "Version 1.000"
    for record in list(name.names):
        if record.nameID in RENAMED_IDS:
            name.removeNames(nameID=record.nameID)
    name.setName(family, 1, 3, 1, 0x409)
    name.setName("Regular", 2, 3, 1, 0x409)
    name.setName(f"{version};{postscript};OpenChat static instance", 3, 3, 1, 0x409)
    name.setName(family, 4, 3, 1, 0x409)
    name.setName(postscript, 6, 3, 1, 0x409)
    # Any other naming record that still carries a Reserved Font Name (axis or
    # instance names left behind by the instancer, feature names) says the new
    # name instead.
    for record in name.names:
        if record.nameID in ATTRIBUTION_IDS:
            continue
        text = record.toUnicode()
        renamed = text
        for reserved, replacement in renames:
            renamed = renamed.replace(reserved, replacement)
        if renamed != text:
            record.string = renamed
    # One regular style per family: the weight is in the family name, so no
    # platform's style matching can pick a different weight or synthesise one.
    if "STAT" in font:
        del font["STAT"]
    os2 = font["OS/2"]
    os2.usWeightClass = 400
    os2.fsSelection = (os2.fsSelection & ~0b1100001) | 0b1000000  # REGULAR, not ITALIC/BOLD
    font["head"].macStyle = 0


def check_reserved_names(font, renames, out):
    """Refuse a cut whose naming records still use a Reserved Font Name."""
    tokens = {reserved.split()[0].lower() for reserved, _ in renames}
    for record in font["name"].names:
        if record.nameID in ATTRIBUTION_IDS:
            continue
        text = record.toUnicode()
        if any(token in text.lower() for token in tokens):
            raise SystemExit(f"{out}: name record {record.nameID} still uses a Reserved Font Name: {text!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, help="directory with fredoka/, orbitron/, playfairdisplay/")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--out", default=os.path.join(here, "..", "..", "assets", "fonts"))
    args = parser.parse_args()

    for folder, source, axes, family, postscript, file_name, renames in JOBS:
        font = TTFont(os.path.join(args.source, folder, source))
        instance = instantiateVariableFont(font, axes)
        rename(instance, family, postscript, renames)
        out_dir = os.path.join(args.out, folder)
        out = os.path.join(out_dir, file_name)
        check_reserved_names(instance, renames, out)
        os.makedirs(out_dir, exist_ok=True)
        instance.save(out)
        licence = os.path.join(args.source, folder, "OFL.txt")
        shutil.copyfile(licence, os.path.join(out_dir, "OFL.txt"))
        print("wrote", os.path.normpath(out), os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
