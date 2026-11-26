"""Write craft.txt for the plugin's craft-from-the-item feature (src/craft.c).

The plugin needs two things per craft: which base items a recipe family applies to, and which rune
and gem it takes beside the jewel. Both come from the Craft Center catalog that pd2-holy-inventory
already builds from the game's own tables, so this only reshapes it:

    base <item code> <category>
    recipe <category> <family> <rune code> <gem code>

Usage:
    python tools/make_craft_table.py [catalog.json] [out.txt]

Without a catalog file it asks the running holy-inventory server for /api/v1/craft/catalog.
"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

RUNES = ("El Eld Tir Nef Eth Ith Tal Ral Ort Thul Amn Sol Shael Dol Hel Io Lum Ko Fal Lem Pul Um "
         "Mal Ist Gul Vex Ohm Lo Sur Ber Jah Cham Zod").split()
RUNE_CODES = {f"{name} Rune": f"r{i + 1:02d}" for i, name in enumerate(RUNES)}
GEM_CODES = {
    "Perfect Amethyst": "gpv", "Perfect Diamond": "gpw", "Perfect Emerald": "gpg",
    "Perfect Ruby": "gpr", "Perfect Sapphire": "gpb", "Perfect Topaz": "gpy", "Perfect Skull": "skz",
}


def load(source: str | None) -> dict:
    if source:
        data = json.loads(Path(source).read_text(encoding="utf-8"))
    else:
        with urllib.request.urlopen("http://127.0.0.1:31415/api/v1/craft/catalog", timeout=30) as r:
            data = json.load(r)
    return data.get("craftCatalog", data)


def build(catalog: dict) -> list[str]:
    lines = ["# Written by tools/make_craft_table.py from the Craft Center catalog."]
    for category in catalog["categories"]:
        key = category["key"]
        for recipe in category["recipes"]:
            rune = next((RUNE_CODES[i] for i in recipe["inputs"] if i in RUNE_CODES), None)
            gem = next((GEM_CODES[i] for i in recipe["inputs"] if i in GEM_CODES), None)
            if not rune or not gem:
                raise SystemExit(f"{key}/{recipe['family']}: cannot read inputs {recipe['inputs']}")
            lines.append(f"recipe {key} {recipe['family']} {rune} {gem}")
        for base in category["bases"]:
            lines.append(f"base {base['code']} {key}")
    return lines


def main() -> None:
    source = sys.argv[1] if len(sys.argv) > 1 else None
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "craft.txt")
    lines = build(load(source))
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out}: {sum(l.startswith('recipe') for l in lines)} recipes, "
          f"{sum(l.startswith('base') for l in lines)} bases")


if __name__ == "__main__":
    main()
