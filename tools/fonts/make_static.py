#!/usr/bin/env python3
"""Cut the static font instances OpenChat's profile pages ship.

Three of the bundled profile faces are published only as variable fonts
(Fredoka, Orbitron, Playfair Display). The renderer selects every face by its
family name alone, on every platform, so it ships the two weights it uses of
each as plain static fonts instead, named after the weight:

    Fredoka Medium, Fredoka SemiBold          (wght 500 / 600, wdth 100)
    Orbitron SemiBold, Orbitron ExtraBold     (wght 600 / 800)
    Playfair Display Regular, Playfair Display Bold   (wght 400 / 700)

Usage:
    python3 tools/fonts/make_static.py --source DIR [--out assets/fonts]

DIR holds the upstream files in the google/fonts layout: fredoka/,
orbitron/ and playfairdisplay/ with their variable .ttf and OFL.txt. The
output goes to OUT/<family>/<Family><Weight>.ttf, beside a copy of the
family's OFL.txt. The copyright, licence and version records of the source
are kept; only the naming records change. Needs fontTools (pip install
fonttools).
"""
import argparse
import os
import shutil

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

JOBS = [
    ("fredoka", "Fredoka[wdth,wght].ttf", {"wght": 500, "wdth": 100}, "Fredoka Medium"),
    ("fredoka", "Fredoka[wdth,wght].ttf", {"wght": 600, "wdth": 100}, "Fredoka SemiBold"),
    ("orbitron", "Orbitron[wght].ttf", {"wght": 600}, "Orbitron SemiBold"),
    ("orbitron", "Orbitron[wght].ttf", {"wght": 800}, "Orbitron ExtraBold"),
    ("playfairdisplay", "PlayfairDisplay[wght].ttf", {"wght": 400}, "Playfair Display Regular"),
    ("playfairdisplay", "PlayfairDisplay[wght].ttf", {"wght": 700}, "Playfair Display Bold"),
]

# Naming records rewritten for the instance: family, unique id, full name,
# PostScript name, typographic family/subfamily, WWS names and the variations
# PostScript prefix. Everything else (copyright, licence, version) stays.
RENAMED_IDS = (1, 2, 3, 4, 6, 16, 17, 21, 22, 25)


def rename(font, family):
    name = font["name"]
    version = name.getDebugName(5) or "Version 1.000"
    postscript = family.replace(" ", "")
    for record in list(name.names):
        if record.nameID in RENAMED_IDS:
            name.removeNames(nameID=record.nameID)
    name.setName(family, 1, 3, 1, 0x409)
    name.setName("Regular", 2, 3, 1, 0x409)
    name.setName(f"{version};{postscript};OpenChat static instance", 3, 3, 1, 0x409)
    name.setName(family, 4, 3, 1, 0x409)
    name.setName(postscript, 6, 3, 1, 0x409)
    # One regular style per family: the weight is in the family name, so no
    # platform's style matching can pick a different weight or synthesise one.
    if "STAT" in font:
        del font["STAT"]
    os2 = font["OS/2"]
    os2.usWeightClass = 400
    os2.fsSelection = (os2.fsSelection & ~0b1100001) | 0b1000000  # REGULAR, not ITALIC/BOLD
    font["head"].macStyle = 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, help="directory with fredoka/, orbitron/, playfairdisplay/")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--out", default=os.path.join(here, "..", "..", "assets", "fonts"))
    args = parser.parse_args()

    for folder, source, axes, family in JOBS:
        font = TTFont(os.path.join(args.source, folder, source))
        instance = instantiateVariableFont(font, axes)
        rename(instance, family)
        out_dir = os.path.join(args.out, folder)
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, family.replace(" ", "") + ".ttf")
        instance.save(out)
        licence = os.path.join(args.source, folder, "OFL.txt")
        shutil.copyfile(licence, os.path.join(out_dir, "OFL.txt"))
        print("wrote", os.path.normpath(out), os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
