"""Write craft.txt for the plugin's craft-from-the-item feature (src/craft.c, src/item_menu.c).

The plugin needs, per item type: which recipe families apply, which rune and gem each takes beside
the jewel, what each is sure to add (shown in the item menu, short), and whether the type can be
corrupted and how often that bricks it. All of it comes from the Craft Center catalog that
pd2-holy-inventory already builds from the game's own tables, so this only reshapes it:

    base <item code> <category>
    recipe <category> <family> <rune code> <gem code>
    mods <category> <family> <short text to the end of the line>
    corrupt <category> <brick percent>

Weapons and quivers are not in the catalog — the site leaves them out for reasons of its own (a
corruption table split by weapon class, a damage readout it lacks), neither of which matters to
the menu — so they are read straight from the game's tables in pd2data.mpq: CubeMain's craft rows
for "weap" and "misl", Weapons.txt for the bases, ItemTypes.txt to tell a quiver from a weapon.

Usage:
    python tools/make_craft_table.py [catalog.json] [out.txt] [pd2data.mpq]

Without a catalog file it asks the running holy-inventory server for /api/v1/craft/catalog.
"""
from __future__ import annotations

import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pd2_offline_addons import mpq_asset  # noqa: E402

DEFAULT_MPQ = Path(r"C:\Users\musae\Games\PC\Diablo II\ProjectD2\Live\pd2data.mpq")

RUNES = ("El Eld Tir Nef Eth Ith Tal Ral Ort Thul Amn Sol Shael Dol Hel Io Lum Ko Fal Lem Pul Um "
         "Mal Ist Gul Vex Ohm Lo Sur Ber Jah Cham Zod").split()
RUNE_CODES = {f"{name} Rune": f"r{i + 1:02d}" for i, name in enumerate(RUNES)}
GEM_CODES = {
    "Perfect Amethyst": "gpv", "Perfect Diamond": "gpw", "Perfect Emerald": "gpg",
    "Perfect Ruby": "gpr", "Perfect Sapphire": "gpb", "Perfect Topaz": "gpy", "Perfect Skull": "skz",
}

# How each mod reads in the menu: a short name, and whether its range is a percentage. A mod not
# listed falls back to the catalog's own text, which is long but never wrong.
SHORT = {
    "ac%": ("ED", True), "ac-hth": ("Def vs melee", False), "ac-miss": ("Def vs missile", False),
    "all-stats": ("All attributes", False), "att": ("AR", False), "att%": ("AR", True),
    "balance1": ("FHR", True), "block": ("Block", True), "block2": ("FBR", True),
    "cast1": ("FCR", True), "cast2": ("FCR", True), "crush": ("CB", True),
    "curse-res": ("Curse duration -", True), "deadly": ("DS", True), "dex": ("Dex", False),
    "dmg-max": ("Max dmg", False), "dmg-min": ("Min dmg", False), "enr": ("Energy", False),
    "gold%": ("Gold", True), "heal-kill": ("Life/kill", False), "hp": ("Life", False),
    "hp%": ("Life", True), "lifesteal": ("LL", True), "mag%": ("MF", True), "mana": ("Mana", False),
    "mana%": ("Mana", True), "mana-kill": ("Mana/kill", False), "manasteal": ("ML", True),
    "move1": ("FRW", True), "move2": ("FRW", True), "openwounds": ("OW", True),
    "pierce": ("Pierce", True), "red-dmg": ("DR", False), "red-dmg%": ("PDR", True),
    "red-mag": ("MDR", False), "reduce-ac": ("Target def", False), "regen": ("Regen", False),
    "regen-mana": ("Mana regen", True), "res-all": ("Res", False), "res-cold": ("Cold res", True),
    "res-fire": ("Fire res", True), "res-ltng": ("Light res", True), "res-pois": ("Poison res", True),
    "res-pois-len": ("PLR", True), "str": ("Str", False), "swing2": ("IAS", True), "vit": ("Vit", False),
}
SHORT.update({
    "dmg%": ("Dmg", True), "dmg-undead": ("Dmg undead", True), "dmg-demon": ("Dmg demon", True),
    "balance2": ("FHR", True), "reduce-ac": ("Target def -", True),
})
FIXED = {"gethit-skill": "Nova when struck", "half-freeze": "Half freeze", "knock": "Knockback",
         "allskills": "+1 skills"}


def short(mod: dict) -> str:
    code = mod["code"]
    if code in FIXED:
        return FIXED[code]
    if code not in SHORT:
        return mod["text"]
    name, percent = SHORT[code]
    lo, hi = mod["lo"], mod["hi"]
    span = f"{lo}" if lo == hi else f"{lo}-{hi}"
    return f"{name}{'' if name.endswith('-') else ' '}{span}{'%' if percent else ''}"


def load(source: str | None) -> dict:
    if source:
        data = json.loads(Path(source).read_text(encoding="utf-8"))
    else:
        with urllib.request.urlopen("http://127.0.0.1:31415/api/v1/craft/catalog", timeout=30) as r:
            data = json.load(r)
    return data.get("craftCatalog", data)


GEM_GROUPS = {"gg4a": "gpv", "gg4d": "gpw", "gg4e": "gpg", "gg4r": "gpr", "gg4s": "gpb", "gg4t": "gpy",
              "gg4z": "skz"}
FAMILY_OF_GEM = {"gpv": "caster", "gpw": "brilliant", "gpg": "safety", "gpr": "blood", "gpb": "hitpower",
                 "gpy": "bountiful", "skz": "vampiric"}


def table(archive: Path, name: str) -> list[dict]:
    raw = mpq_asset.read(archive, f"data\\global\\excel\\{name}.txt")
    if not raw:
        raise SystemExit(f"{name}.txt not found in {archive}")
    lines = raw.decode("latin-1").splitlines()
    head = lines[0].split("\t")
    return [dict(zip(head, line.split("\t"))) for line in lines[1:] if line.strip()]


def tokens(text: str) -> list[str]:
    return [t for t in text.strip('"').split(",") if t]


def weapon_lines(archive: Path) -> list[str]:
    """Weapons ("weap") and quivers ("misl"): bases, the craft families, and corruption."""
    types = {row.get("Code"): row for row in table(archive, "itemtypes") if row.get("Code")}

    def ancestors(code: str) -> set[str]:
        seen, todo = set(), [code]
        while todo:
            c = todo.pop()
            if not c or c in seen:
                continue
            seen.add(c)
            row = types.get(c, {})
            todo += [row.get("Equiv1", ""), row.get("Equiv2", "")]
        return seen

    lines: list[str] = []
    cube = table(archive, "cubemain")
    for root in ("weap", "misl"):
        done = set()
        for row in cube:
            first = tokens(row.get("input 1", ""))
            if (row.get("enabled") != "1" or "crf" not in row.get("output", "") or not first
                    or first[0] != root or "mag" not in first or "eth" in first):
                continue
            ins = [tokens(row.get(f"input {i}", ""))[:1] for i in range(2, 5)]
            ins = [i[0] for i in ins if i]
            rune = next((i[:3] for i in ins if i.startswith("r") and i[1:3].isdigit()), None)
            gem = next((GEM_GROUPS[i] for i in ins if i in GEM_GROUPS), None)
            if not rune or not gem:
                continue   # the "craft ingredient" rows: one item instead of the three
            family = FAMILY_OF_GEM[gem]
            if family in done:
                continue
            done.add(family)
            mods = []
            for i in range(1, 6):
                code = row.get(f"mod {i}", "")
                if not code or code == "ethereal":
                    continue
                mods.append(short({"code": code, "lo": int(row.get(f"mod {i} min") or 0),
                                   "hi": int(row.get(f"mod {i} max") or 0), "text": code}))
            lines.append(f"recipe {root} {family} {rune} {gem}")
            lines.append(f"mods {root} {family} " + ", ".join(mods))

        # Corruption: the share of the 1..1000 roll that lands on a row with no outcome — the
        # catalog's own reckoning, for the rows that name this root and nothing narrower.
        rows = []
        for row in cube:
            first = tokens(row.get("input 1", ""))
            if (row.get("enabled") != "1" or row.get("param") != "361" or row.get("op") != "16"
                    or not first or first[0] != root or set(first[1:]) - {"nos"}):
                continue
            outcome = [row.get(f"mod {i}") for i in range(1, 6)
                       if row.get(f"mod {i}") and row.get(f"mod {i}") not in {"corrupt", "corruptnum"}]
            rows.append((int(row.get("value") or 0), bool(outcome)))
        if rows:
            brick = 0
            for roll in range(1, 1001):
                hit = next((has for threshold, has in rows if roll < threshold), True)
                brick += 0 if hit else 1
            lines.append(f"corrupt {root} {round(brick / 10)}")

    # Arrows and bolts are misc items in D2, not weapons.
    for row in table(archive, "weapons") + table(archive, "misc"):
        code, kind = row.get("code", ""), row.get("type", "")
        if not code or not kind or row.get("spawnable") != "1":
            continue
        family = ancestors(kind)
        root = "misl" if "misl" in family else "weap" if "weap" in family else None
        if root:
            lines.append(f"base {code} {root}")
    return lines


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
            lines.append(f"mods {key} {recipe['family']} " + ", ".join(short(m) for m in recipe["mods"]))
        if category.get("corruption"):
            lines.append(f"corrupt {key} {round(100 * category.get('brickShare', 0))}")
        for base in category["bases"]:
            lines.append(f"base {base['code']} {key}")
    return lines


def main() -> None:
    source = sys.argv[1] if len(sys.argv) > 1 else None
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "craft.txt")
    archive = Path(sys.argv[3]) if len(sys.argv) > 3 else DEFAULT_MPQ
    lines = build(load(source)) + weapon_lines(archive)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out}: {sum(l.startswith('recipe') for l in lines)} recipes, "
          f"{sum(l.startswith('base') for l in lines)} bases, "
          f"{sum(l.startswith('corrupt') for l in lines)} corruptible types")


if __name__ == "__main__":
    main()
