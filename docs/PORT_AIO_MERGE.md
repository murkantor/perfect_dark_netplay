# All-in-One (AIO) upstream merges

Record of syncing `jonaeru/perfect_dark` branch `port-mods/all-in-one` (remote
`jonaeru`) into `port-net-predict`, the places where we deliberately differ from
AIO, and how to do the next sync. Read before the next AIO merge, or when
touching `g_MpBodies`, the mpsetups.bin format, or stage numbers past 0x5b.

## How to merge (read first)

**Do not `git merge jonaeru/port-mods/all-in-one` directly.** fgsfds rebased the
upstream `port` history in 2026, so AIO holds the old commits and we hold the
rewritten ones. `git merge-base --all` returns three bases (criss-cross) and the
virtual base produces ~60 fake conflicts in files neither side really changed.

What worked (2026-10-17):

1. `git fetch jonaeru port-mods/all-in-one`, then work in a worktree (the main
   tree usually has uncommitted WIP).
2. `git log --graph HEAD..jonaeru/port-mods/all-in-one` and drop every commit
   whose *subject* already exists in HEAD (`git log --grep=... HEAD`). The
   rebased fgsfds commits have different patch-ids, so `git cherry` marks them
   all `+`; compare by subject, then check the code.
3. `git cherry-pick -x` the rest oldest-first and resolve each one.
4. Record the sync with a merge commit (`git merge -s ours`) whose tree is the
   cherry-picked result. **The next sync's base is that merge commit**, so only
   AIO commits made after it show up in `HEAD..jonaeru/...`.

## 2026-10-17 sync (AIO tip d3e42050f)

Picked: 10 new arenas, sky refactor, ext-texture bounds fixes, KillMaster
medal fix, file-slot expansion, ext-texture/menu fixes, memory expansion for
models, ~75 new MP bodies + ~80 heads (GE-X characters, `mpplayers.bin` for
full-width player head/body), 5 music tracks, auto head select (hold R), random
sim select, neck-height auto-adjust, texture surfacetype override/restore,
solo/co-op mod-file reset, MP-end bg-file pin crash fix, Explosive Sims, rename
crash fix; plus fgsfds `port` fixes newer than our last sync (Wayland keyboard,
mousewheel menu scrolling, All Guns cheat in MP, vertical camera wrap fix,
camspy light LOS, freedesktop renames).

Skipped because they are already in our tree under rebased hashes: three-point
filter fixes (8aaca2129, 3d372f1f2, 783060f49 = our fgsfds #718), menu texture
corruption fix (c80360083 = 998eee9c6), and every pre-May 2026 fgsfds commit.
Skipped because superseded: d3e42050f (zero block version), folded into
mpsetups v11.

## Deliberate deviations from AIO

| Area | AIO | Ours | Why |
|---|---|---|---|
| `g_MpBodies` 0x39-0x3f | Skedar, Caroll, Caroll Sinister inserted at 0x39; Connery..DJBond pushed to 0x3c-0x3f | Connery..DJBond stay at N64 0x39-0x3c; Skedar 0x3d, BODY_TESTCHR (Sinister) 0x3e, BODY_CARROLL3 (Caroll) 0x3f | `mpbodynum` is saved and sent on the wire as a raw index (netplay review M-2). From 0x40 on, indices match AIO. |
| `MPBODY_CONNERY..DJBOND` | 0x3c-0x3f on PC | 0x39-0x3c everywhere | Must match the table. Our pre-merge PC values (0x3b-0x3e) were a leftover from the M-2 reorder, so DarkSim/Demon profiles picked the wrong body. |
| Explosive Sims bit | `MPOPTION_EXPLODESIMONDEATH 0x10000000` | options bit 53 (`0x0020000000000000ULL`), high-word checkbox handler | 0x10000000 is RESERVED here (retired NOCULL). |
| Explosive Sims sync | local `explosionCreateSimple` | server-only (`ownsStats`) + immediate `SVC_EXPLOSION` | Clients must not roll their own blasts. |
| mpsetups.bin | v2 = per-block version trailer of MPSETUP_MAXSETUPS bytes; 8-bit bot head/body gated on block version > 1 | **v11**: 96-byte blocks, `numsetups` block versions right after the blocks, 8-bit head/body gated on block version >= 11 | Our v2-v10 already mean other things (weapon presets ... scenario high bit), and our 80-byte block was 100% full. Files are not interchangeable with AIO's. |
| `chrdata.headnum` | s16 | s16, in our repacked `chrdata` | Same change, different struct layout. |
| Stage numbers | STAGE_TITLE/BOOTPAKMENU/CREDITS/4MBMENU moved to 0x66-0x69 | same | Port literals fixed: `scripts/chaos.lua` MENU_STAGES, `main.c` --boot-stage bound. |

`NET_PROTOCOL_VER` 89 -> 90 (stage/body/head tables, option bit 53): deploy the
VPS server and clients together.

## mpsetups.bin v11 layout

```
[version][defaultsetup][numsetups]
numsetups x [block]          80 bytes if version <= 10, else MPSETUP_BLOCKSIZE (96)
numsetups x [blockversion]   v11+ only
[weapon presets tail]        v2+, g_MpSetupFile only
```

- Each block is decoded with **its own** version (`mpsetupLoadSetup`). A block
  keeps its version until it is re-saved. This also fixed an old bug: saving any
  setup used to re-stamp the whole file at the current version while older
  blocks kept their old layout.
- Blocks from v1-10 files are zero-filled past byte 80. Every version-gated wad
  field reads zero as "absent".
- Missing, zero or future block versions fall back to the file version.
- 656/768 bits are used. A new field goes at the end of `mpsetupfileSaveWad`
  behind a `version >= 12` read gate.

## Gotchas

- **The AIO data package must match.** New characters, arenas and music need
  the matching AIO data files (models, bg files, sequences, and `lang` strings:
  AIO re-uses string slots such as `L_OPTIONS_086`/`L_OPTIONS_355`). With stale
  data, the labels show the wrong text and an external-only model slot fails to
  load (`romdataFileLoad: external-only file ... not found`).
- `romdataFileFreeForSolo` now resets **every** file slot (except expansion and
  language files) when a solo mission starts. The chain ROM (`--mod-rom`) still
  returns early.
- `NUM_MODS` (5) counts the loose-file mod slots only. `MOD_CHAINROM`/`MOD_COUNT`
  are ours, and the chain table mirrors the expansion slots from `expansionFiles[]`
  (so `romdataInitFileSlotExpansion` must run before `romdataInitChainFiles`).
- AIO's TODO: "Multiple Tunes" is a 48-bit mask, so tracks past 0x2f can't be
  in the random-tune set.

## Status

Compile-verified only. Runtime checklist:

1. Load an existing (v10) mpsetups.bin: setup names, bots, Lives/Race settings
   are intact. Re-save one setup and reload. Others still load.
2. Bot with a GE-X body (index >= 0x80) + GE-X head: save setup, reload, same.
3. Player profile with a new head: save, reload (`mpplayers.bin`).
4. Each new arena loads, plus the title, credits and CI hub (stage renumber).
5. Chaos off on the title/credits screens; chaos on in a new arena.
6. Netplay: Explosive Sims on, a client sees the blast and takes the damage.
7. Solo mission after an MP match with an AIO mod: no crash, stock data.
