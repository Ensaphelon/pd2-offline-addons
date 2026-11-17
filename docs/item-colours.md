# Item colours

Drawing a unique item in another colour, through the game's own palette-shift column rather than
through anything added to it.

```sh
python3 -m pd2_offline_addons recolour --game "<path to>/ProjectD2/Game.exe"
python3 -m pd2_offline_addons recolour --game "…" --colour Toothrow=blac
python3 -m pd2_offline_addons uncolour --game "<path to>/ProjectD2/Game.exe"
```

`status` reports the colour the archive actually holds, beside the one PD2 ships:

```
colour     Toothrow: dgry  was bwht
```

The 21 codes are Colors.txt's, and they are read out of the game rather than written down in the
installer:

```
whit lgry dgry blac   lblu dblu cblu   lred dred cred
lgrn dgrn cgrn   lyel dyel   lgld dgld   lpur dpur   oran bwht
```

## What PD2 does and does not offer

PD2's cosmetic feature is **Alternate Item Skins** — a separate drop carrying its own artwork, for
a fixed list of items. It is not a dye, there is no colour to choose, and Toothrow is not on the
list. Nor is there a hidden one: `transform_dye` exists as stat 477 with five bits, and
`Properties.txt` has a `dye` code, but the string table carries no dye or cosmetic text at all,
ProjectDiablo.dll carries none either, and the `dye` property appears in exactly two internal cube
recipes. So a colour choice has to come from the item tables.

## The column that actually does it

Two columns in `UniqueItems.txt`, each holding a four-letter code from `Colors.txt`:

| column | what it colours |
|---|---|
| `chrtransform` | the item on the character |
| `invtransform` | the inventory sprite |

351 of PD2's 485 uniques set them. Toothrow sets `bwht` (Bright White) and `whit` (White) — the
brightest pair there is, which is why it glares rather than reads as bone.

`Armor.txt`'s `Transform` and `InvTrans` are **not** this. They take six distinct values across the
whole file (0, 1, 2, 5, 7, 8) against Colors.txt's 21, because they select a palette *group* and
not a shade. Changing them recolours nothing, and it would apply to every Sharktooth Armor rather
than to the unique.

## Why the compiled table, and not the .txt

The game reads the compiled form. `UniqueItems.bin` sits in pd2data.mpq beside the .txt, and it is
PD2's own rebuild rather than a leftover: 484 records where stock 1.13 has 403, carrying PD2's own
uniques. A loose `.txt` is therefore read only with `-direct -txt`, and PD2Launcher can pass
neither — its whole flag set is graphics/skiptobnet/sndbkg/disableAutoUpdate. Patching the compiled
table keeps the game being started the way it always is, which is the same reason the RESTART label
goes into an archive.

## How the two bytes were found

Not off a struct listing. The record layout was established from the file itself:

* the first four bytes are the record count — 484, and `4 + 484 × 332` is exactly the file's length
* each record carries its name at offset 2, null-terminated, 32 bytes
* every one of the 332 byte offsets was then tested against what the .txt says that item's colour
  is, across all 484 records at once

Offset 56 agrees with `chrtransform` on every record and no other offset does; 57 likewise with
`invtransform`. An item that sets no colour reads 0xFF. All three facts are re-checked at install
time, so a PD2 update that adds a column is refused rather than written over — the failure mode
being guarded against is not a crash but setting *something other than a colour* on the wrong item,
across a whole save.

## Why the file can go in stored, uncompressed

`mpq_asset.add` writes files stored, with no sector offset table, and this one is 160,692 bytes —
forty sectors' worth. That is not a guess about what Storm.dll tolerates: Blizzard's own
`d2data.mpq` holds 3,758 stored files larger than one sector, the largest 1.36 MB, and `d2exp.mpq`
holds 6,306. The game reads them every session.

Worth knowing if a reader ever disagrees: a stored file has no offset table, and a reader that
assumes one — pd2-holy-inventory's own, for instance — reads the first sector offsets as data and
returns a short file. PD2 compresses everything it ships, so such a reader never meets the case
until something is added to an archive.

## What it leaves behind

pd2data.mpq is backed up to `pd2data.mpq.pd2addons-backup` on the first write and never again, so
recolouring twice cannot make the backup a recoloured one. Each run rebuilds from that backup, so
a second colour replaces the first instead of layering on it. `uncolour` is a file copy, and
`uninstall` does it too — a full uninstall leaves nothing of ours in the archive.

A PD2 update replaces pd2data.mpq and silently takes the colour with it, exactly as it does the
RESTART label. That is why `status` reads the archive instead of remembering what it wrote.

Offline only: the table is part of what a realm checks, so a recoloured archive is not something to
take to the ladder.
