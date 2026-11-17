r"""Naming an item's affixes, and the range each one rolls in, inside the game's own tooltip.

A magic, rare or crafted item's tooltip says what its affixes DO but never which affixes they
were or how well they rolled. "+290 to Attack Rating against Undead" could be the top of its
range or the bottom; the item does not say, and that is exactly what decides whether a craft or a
rare is worth keeping. The ids are in the item itself and the loot filter can read them, so the
ranges can be put on screen beside them.

What this does NOT do: attribute a VALUE to an affix. The item stores one merged total per stat
plus the affix ids; the per-affix split is not stored anywhere, and when two affixes feed the
same stat (routine on rares and crafts) it cannot be recovered from the total at all. The filter
can only print a total, so printing "290 of 251-325" would be a lie on exactly the items that
matter most. The rolled value is already on the item's own lines directly above this block; what
is missing is the range, and that is what this adds.

Why a generated block. The filter language only compares an affix id for equality
(`SUFFIX=873`), so putting a name on screen means one rule per affix: PD2 has 871 spawnable
prefixes and 746 spawnable suffixes. Nobody writes that by hand, which is why neither mainstream
filter does it (measured 2026-11-17: Mystery-Hyper and Kassahi's variant use `PREFIX` zero times
between them, and `SUFFIX` for 19 ids, all caster-weapon mods).

Three things about the filter language this rests on, each checked rather than assumed:
  * `{...}` is the item DESCRIPTION, and it shows in the inventory tooltip, not just on the
    ground. The proof is in the user's own filter: a rule on `box` (the Horadric Cube) appends
    "Send Feedback via PD2 Discord" to it, and the cube never lies on the ground.
  * `%NAME%` INSIDE the braces is the description accumulated so far, so a rule can append and
    `%CONTINUE%` on. 2513 rules in that same filter do this, one of them putting `%NAME%` last
    to prepend instead. `%CL%` is a newline that collapses if the text is already at one.
  * an id is tested against every affix slot, not one. The wiki's own example is
    `ItemDisplay[RARE SUFFIX=662 SUFFIX=158]`, which means nothing otherwise.

No quality condition is needed. `PREFIX`/`SUFFIX` are simply absent on set and unique items,
whose quality branch stores a set/unique id where the affixes would be, so one rule set covers
magic, rare and crafted and silently skips everything else.

The id is a ZERO-based row index into MagicPrefix.txt / MagicSuffix.txt, each table counted on
its own. Settled from the live game, not the wiki (whose two statements about the numbering
contradict each other), and settled by VALUES, because names cannot settle it: affixes come in
tiers that share a name, so reading the id as a 1-based row made `SUFFIX=873/899/906` come out as
"of Nova" and "of Damage Amplification" twice -- matching exactly what the season-14 filter
labels those ids (`a3 nov`, `a3 amp`) -- and was still wrong by one.

What decided it: for every rare and crafted item in a real save, take each stat that exactly one
of the item's own affixes can grant and ask whether the value falls inside that affix's min..max.
Zero-based scored 67.6% on gear and 90% over everything; every other offset from -2 to +3 scored
between 1.5% and 26%. The residue is stats fed by more than one affix, plus packed stats like
`item_addskill_tab`, which that test does not try to split. One real crafted Light Plate showed
the off-by-one directly: tohit 106 against "Gold" 81..100, armour 27 against "Sturdy" 10..20 and
damage reduction 3 against "of Protection" 2..2 -- all three outside, and all three inside under
"Platinum" 101..120, "Sturdy" 21..30 and "of Absorption" 3..3.

Each line prints the id as well as the name, so the mapping keeps checking itself in game: a name
that does not fit what the item does is one constant offset away, not a scramble.
"""
from __future__ import annotations

import json
import re
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path

from . import mpq_asset

LOOT_FILTER_NAME = "loot.filter"

BEGIN = "// >>> pd2-offline-addons affix labels >>>"
END = "// <<< pd2-offline-addons affix labels <<<"
BACKUP_SUFFIX = ".pd2addons-backup"

EXCEL = "data\\global\\excel\\"
STRING_TABLES = (
    "data\\local\\lng\\eng\\string.tbl",
    "data\\local\\lng\\eng\\expansionstring.tbl",
    "data\\local\\lng\\eng\\patchstring.tbl",
)
# Load order: later archives override earlier keys, which is the game's own order. The classic
# archives sit one level above PD2's, which is where the base string tables actually live.
_BASE_ARCHIVES = ("d2data.mpq", "d2exp.mpq", "patch_d2.mpq")
_MOD_ARCHIVES = ("patch_d2.mpq", "pd2data.mpq")

# A few Properties.txt codes name no stat at all -- the property function writes them in the
# game's code instead. 32 of 458 codes are like this; these are the ones affixes actually use.
_FUNCTION_STATS = {
    "5": ("mindamage", "maxdamage"),
    "6": ("mindamage", "maxdamage"),
    "7": ("item_mindamage_percent", "item_maxdamage_percent"),
}

# D2 inlines its own colour escapes into table text: 0xFF, then 'c', then a digit. PD2's own
# added strings carry the same escape UTF-8 encoded, so after a latin-1 decode it arrives as
# "\xc3\xbfc3" rather than "\xffc3" -- both forms have to go, or map affixes read "c0Monster
# Density:c3".
_COLOUR = re.compile("(?:\xc3\xbf|\xff)c?.", re.DOTALL)
# printf placeholders in a string-table template. A phrase carrying them is not a simple
# "the number goes here" template (the skill-on-striking ones take three arguments), so it is
# not worth mangling into one -- the property code says what it is unambiguously.
_PRINTF = re.compile(r"%[-0-9.]*[dsiu]|%%")
_FIRST_RULE = re.compile(r"^\s*ItemDisplay\s*\[", re.IGNORECASE)
# Keep a tooltip line readable; D2 does not wrap one, it runs off.
_LINE_BUDGET = 104
# How many of the host filter's own rules the block sits behind; see _insertion_point.
_SKIP_OPENING_RULES = 1000


class AffixLabelError(Exception):
    pass


@dataclass(frozen=True)
class LabelStatus:
    """Read out of the filter file itself. PD2Launcher rewrites loot.filter from whichever filter
    is selected, which takes the block with it -- a remembered flag would keep claiming it is in."""

    filter_path: Path | None
    installed: bool
    rules: int
    backup_present: bool


def _parse_tbl(blob: bytes) -> dict[str, str]:
    """A Diablo II .tbl string table: header, index list, then 17-byte hash entries.

    Ported from pd2-holy-inventory's own reader, where it has been resolving this game's strings
    for months. The fields that matter are the used flag and the two string offsets.
    """
    out: dict[str, str] = {}
    if len(blob) < 21:
        return out
    elements = struct.unpack_from("<H", blob, 2)[0]
    hash_size = struct.unpack_from("<I", blob, 4)[0]
    base = 21 + elements * 2

    def text_at(offset: int) -> str:
        end = blob.find(b"\x00", offset)
        if offset <= 0 or end < 0:
            return ""
        return blob[offset:end].decode("latin-1", "replace")

    for index in range(hash_size):
        entry = base + index * 17
        if entry + 17 > len(blob):
            break
        if not blob[entry]:
            continue
        key_at, value_at = struct.unpack_from("<II", blob, entry + 7)
        key = text_at(key_at)
        if key:
            out[key.lower()] = text_at(value_at)
    return out


def _table(archive: Path, name: str) -> list[dict[str, str]]:
    raw = mpq_asset.read(archive, EXCEL + name + ".txt")
    if raw is None:
        raise AffixLabelError(f"{archive.name} has no {name}.txt")
    lines = raw.decode("latin1").replace("\r\n", "\n").strip("\n").split("\n")
    header = lines[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in lines[1:]]


def game_strings(install_dir: Path) -> dict[str, str]:
    """The game's own display strings, merged in load order."""
    merged: dict[str, str] = {}
    candidates = [install_dir.parent / name for name in _BASE_ARCHIVES]
    candidates += [install_dir / name for name in _MOD_ARCHIVES]
    for archive in candidates:
        if not archive.is_file():
            continue
        for table in STRING_TABLES:
            try:
                blob = mpq_asset.read(archive, table)
            except mpq_asset.MpqError:
                continue
            if blob:
                merged.update(_parse_tbl(blob))
    if not merged:
        raise AffixLabelError(f"found no string tables under {install_dir}")
    return merged


def _clean(text: str) -> str:
    """ANSI, one line, and none of the parser's own punctuation.

    `%` has to go whatever it costs: it opens a token, so one left in a name would make the rest
    of the line parse as something else entirely.
    """
    stripped = _COLOUR.sub("", text).replace("\n", " ").replace("\t", " ")
    kept = "".join(ch for ch in stripped if 32 <= ord(ch) < 127)
    kept = kept.replace("{", "(").replace("}", ")").replace("%", "")
    return re.sub(r"\s+", " ", kept).strip(" :")


def stat_phrases(install_dir: Path, archive: Path) -> dict[str, tuple[str, int]]:
    """stat name -> (the game's own wording, where the number goes).

    Placement is not guessed: `descval` in ItemStatCost is the game's own answer -- 1 puts the
    number before the phrase ("+120 to Attack Rating"), 2 after it ("Physical Damage Taken
    Reduced by 3"), and 0 means the phrase carries no number at all ("Ignore Target's Defense").
    Getting this from the table is the difference between that and "3 Physical Damage Taken
    Reduced by".
    """
    strings = game_strings(install_dir)
    out: dict[str, tuple[str, int]] = {}
    for row in _table(archive, "ItemStatCost"):
        stat = (row.get("Stat") or "").strip()
        key = (row.get("descstrpos") or "").strip().lower()
        if not stat or not key:
            continue
        phrase = strings.get(key)
        if not phrase or _PRINTF.search(phrase):
            continue
        raw = (row.get("descval") or "").strip()
        out[stat] = (_clean(phrase), int(raw) if raw.isdigit() else 1)
    return out


def property_stats(archive: Path) -> dict[str, tuple[str, ...]]:
    """Properties.txt code -> the stats it writes."""
    out: dict[str, tuple[str, ...]] = {}
    for row in _table(archive, "Properties"):
        code = (row.get("code") or "").strip()
        if not code:
            continue
        named = tuple(row.get(f"stat{i}", "").strip() for i in range(1, 8)
                      if row.get(f"stat{i}", "").strip())
        out[code] = named or _FUNCTION_STATS.get((row.get("func1") or "").strip(), ())
    return out


def _wording(code: str, phrases: dict[str, tuple[str, int]],
             prop_stats: dict[str, tuple[str, ...]]) -> tuple[str, int] | None:
    """The game's wording for a property, as one phrase however many stats it writes.

    `res-all` writes four: fire, lightning, cold and poison resistance, all from the same roll.
    Taking the first would label it "Fire Resist" and hide three quarters of what the affix does,
    so the wordings are folded on what they have in common -- "All Resist" -- which is both
    shorter and true. Where they have nothing in common they are simply joined.
    """
    found: list[tuple[str, int]] = []
    for stat in prop_stats.get(code, ()):
        phrase = phrases.get(stat)
        if phrase and phrase not in found:
            found.append(phrase)
    if not found:
        return None
    if len(found) == 1:
        return found[0]
    common: list[str] = []
    for tail in zip(*(phrase.split()[::-1] for phrase, _ in found)):
        if len(set(tail)) != 1:
            break
        common.append(tail[0])
    place = found[0][1]
    if common:
        return ("All " + " ".join(reversed(common)), place)
    return (" / ".join(phrase for phrase, _ in found), place)


def _mod_text(code: str, low: str, high: str, phrases: dict[str, tuple[str, int]],
              prop_stats: dict[str, tuple[str, ...]]) -> str:
    """One mod, as the game would word it, with the range where the number goes."""
    low, high = low.strip(), high.strip()
    # Some mods carry no roll at all -- `rep-dur` takes a fixed rate out of `param` -- and
    # printing an empty range for them leaves a dangling label.
    span = "" if not (low or high) else (low if low == high else f"{low}-{high}")
    found = _wording(code, phrases, prop_stats)
    if found:
        phrase, place = found
        if "#" in phrase:
            return phrase.replace("#", span, 1).strip()
        if place == 2 or not span:
            return f"{phrase} {span}".strip()
        if place == 0:
            # The phrase is the whole mod ("Ignore Target's Defense"); a range would be noise,
            # unless the affix really does roll one.
            return phrase if low == high else f"{phrase} ({span})"
        return f"{span} {phrase}".strip()
    # No wording in the tables. The stat's own name still says what it does, and reads better
    # than the property code; only properties that name no stat at all fall back to the code.
    stats = prop_stats.get(code, ())
    if stats:
        name = re.sub(r"^(?:item|passive)_", "", stats[0]).replace("_", " ")
        return f"{name} {span}".strip()
    return f"{code} {span}".strip()


def affix_lines(install_dir: Path, archive: Path) -> list[tuple[str, int, str, str]]:
    """(code, id, name, text) per spawnable affix, every one of its mods folded into one line.

    One line per affix rather than per mod, on purpose: a rare carries six affixes of up to three
    mods each, and a tooltip that grows by eighteen lines is worse than no tooltip addition.
    """
    phrases = stat_phrases(install_dir, archive)
    prop_stats = property_stats(archive)
    out: list[tuple[str, int, str, str]] = []
    for code, table, tag in (("PREFIX", "MagicPrefix", "pre"), ("SUFFIX", "MagicSuffix", "suf")):
        for affix_id, row in enumerate(_table(archive, table)):
            # Id 0 is how the item format spells "no affix here": a magic item with no suffix
            # carries suffix 0. A rule on it would label every suffix-less item in the game with
            # whatever sits in row 0, so that row is left alone -- which is also why the game
            # reserves it rather than assigning it.
            if affix_id == 0:
                continue
            if (row.get("spawnable") or "").strip() != "1":
                continue
            name = _clean(row.get("Name") or "")
            if not name:
                continue
            mods = []
            for slot in (1, 2, 3):
                mod = (row.get(f"mod{slot}code") or "").strip()
                if not mod:
                    continue
                mods.append(_mod_text(mod, (row.get(f"mod{slot}min") or "").strip(),
                                      (row.get(f"mod{slot}max") or "").strip(),
                                      phrases, prop_stats))
            if not mods:
                continue
            out.append((code, affix_id, name, _fold(f"{tag} {name} #{affix_id}", mods)))
    return out


def _fold(head: str, mods: list[str]) -> str:
    """`head` then the mods, wrapped onto a continuation line rather than running off screen."""
    lines, current = [], head
    for mod in mods:
        candidate = f"{current}  {mod}" if current == head else f"{current} | {mod}"
        if len(candidate) > _LINE_BUDGET and current != head:
            lines.append(current)
            current = f"    {mod}"
        else:
            current = candidate
    lines.append(current)
    return "%CL%".join(lines)


def generate(install_dir: Path, archive: Path) -> str:
    affixes = affix_lines(install_dir, archive)
    prefixes = sum(1 for code, *_rest in affixes if code == "PREFIX")
    lines = [
        BEGIN,
        "// Generated from MagicPrefix / MagicSuffix / Properties / ItemStatCost and the game's",
        "// own string tables. Do not edit by hand.",
        f"// {prefixes} prefixes + {len(affixes) - prefixes} suffixes, on magic, rare and crafted",
        "// items alike. `python -m pd2_offline_addons unaffixes` takes the block back out.",
    ]
    for code, affix_id, _name, text in affixes:
        lines.append(f"ItemDisplay[{code}={affix_id}]: "
                     f"%NAME%{{%NAME%%CL%%ORANGE%{text}}}%CONTINUE%")
    lines.append(END)
    return "\n".join(lines) + "\n"


def selected_filter(install_dir: Path) -> Path:
    """The filter file to write into, taken from PD2Launcher's own selection.

    Which file matters, because the launcher copies the selected filter over `loot.filter` when
    it starts the game. Write into `loot.filter` while a LOCAL filter is selected and the copy
    lands on top of the block; write into the local file and the copy carries it.

    A local selection records `selectedAuthor.url == "local"` and an absolute path to the file
    under `filters/local/`. Anything else is a filter the launcher downloads, and for those
    `loot.filter` is the only file worth editing -- the launcher replaces the downloaded copy on
    its own schedule, and a block in it would be lost without warning either way.
    """
    settings = install_dir / "AppData" / "launcherSettings.json"
    if settings.is_file():
        try:
            data = json.loads(settings.read_text(encoding="utf-8-sig"))
            chosen = data.get("SelectedAuthorAndFilter", {})
            if (chosen.get("selectedAuthor", {}).get("url") or "").lower() == "local":
                path = Path(chosen.get("selectedFilter", {}).get("path") or "")
                if path.is_file():
                    return path
        except (OSError, ValueError):
            pass
    return install_dir / LOOT_FILTER_NAME


def _read(path: Path) -> str:
    """The file exactly as it is on disk.

    `Path.read_text` grew a `newline` argument only in 3.13, and without it the default
    translates CRLF to LF on the way in -- so the line endings would be sniffed off already
    mangled text and the whole file rewritten with the wrong ones, turning a one-line diff into
    a six-thousand-line one. `open` has taken `newline` since forever.
    """
    with open(path, encoding="latin1", newline="") as handle:
        return handle.read()


def _split(text: str) -> tuple[str, str, str]:
    start = text.find(BEGIN)
    if start < 0:
        return text, "", ""
    stop = text.find(END, start)
    if stop < 0:
        raise AffixLabelError("the filter has our opening marker but no closing one -- it was "
                              "edited by hand; remove the leftover and try again")
    stop += len(END)
    while stop < len(text) and text[stop] in "\r\n":
        stop += 1
    return text[:start], text[start:stop], text[stop:]


def status(filter_path: Path) -> LabelStatus:
    if not filter_path.is_file():
        return LabelStatus(None, False, 0, False)
    _before, block, _after = _split(_read(filter_path))
    return LabelStatus(
        filter_path=filter_path,
        installed=bool(block),
        rules=sum(1 for line in block.splitlines() if _FIRST_RULE.match(line)),
        backup_present=filter_path.with_name(filter_path.name + BACKUP_SUFFIX).is_file(),
    )


def _insertion_point(lines: list[str]) -> int:
    """Where in the host filter the block actually takes effect.

    Not the top, and not the end -- both were measured in the game and both are dead.

    The end is dead for a reason that is in the engine's own code: a matching rule without
    `%CONTINUE%` ends the description chain for that item, and a mainstream filter carries over a
    thousand of them, so a block appended last never runs.

    The top is dead for a reason I have not found. The same rules that work in a filter of their
    own produce nothing when they sit first in a 6000-rule host, while identical copies placed
    after the host's first thousand rules work. Measured by putting one probe rule at seven
    depths of a real filter at once: the copies at rules 1001, 2001, 3001, 4001 and 5001 all
    printed, the copies at rule 1 and at the end did not. The parser drops nothing (it appends
    every line to an ordered list, with no cap), nothing in the skipped range replaces the
    description, and the description itself has no length limit worth the name -- 383 bytes of it
    rendered fine. So this offset is an empirical fact, not an explanation.
    """
    rules = [i for i, line in enumerate(lines) if _FIRST_RULE.match(line)]
    if not rules:
        return len(lines)
    # Far enough in to clear whatever swallows a big host filter's opening rules, and still far
    # ahead of the chain-stoppers that cluster near the end. Only a big filter gets moved: in a
    # small one the block works at the top, which is where it belongs, and skipping a thousand
    # rules it does not have would push it past the end.
    if len(rules) < 2 * _SKIP_OPENING_RULES:
        return rules[0]
    return rules[_SKIP_OPENING_RULES]


def enable(filter_path: Path, install_dir: Path, archive: Path) -> LabelStatus:
    """Puts the block in (replacing one already there), backing the filter up first."""
    if not filter_path.is_file():
        raise AffixLabelError(f"no filter at {filter_path}")
    if not archive.is_file():
        raise AffixLabelError(f"no game data archive at {archive}")

    text = _read(filter_path)
    newline = "\r\n" if "\r\n" in text else "\n"
    before, _block, after = _split(text)
    host = before + after if after else before

    block = generate(install_dir, archive)
    lines = host.splitlines()
    at = _insertion_point(lines)
    rebuilt = lines[:at] + block.splitlines() + lines[at:]

    backup = filter_path.with_name(filter_path.name + BACKUP_SUFFIX)
    if not backup.is_file():
        shutil.copy2(filter_path, backup)
    filter_path.write_text(newline.join(rebuilt) + newline, encoding="latin1", newline="")
    return status(filter_path)


def disable(filter_path: Path) -> bool:
    """Takes the block out, leaving the rest of the filter exactly as it was."""
    if not filter_path.is_file():
        return False
    text = _read(filter_path)
    before, block, after = _split(text)
    if not block:
        return False
    filter_path.write_text(before + after, encoding="latin1", newline="")
    filter_path.with_name(filter_path.name + BACKUP_SUFFIX).unlink(missing_ok=True)
    return True
