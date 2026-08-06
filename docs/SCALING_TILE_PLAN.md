# Tile-Domain 1.5x Scaling — Implementation Plan

**Status: WORKING WIP v13 (2026-08-06). Supersedes the Mode 4 bitmap approach in [SCALING.md](SCALING.md).**

v8-v13 journal: scroll engine (pair-slot ring, eviction), SCALE_FULL vertical
10/9 via HBlank-DMA VOFS tables. Hard-won fixes: menu-toggle reentrancy crash
(sc_busy guard - the sync build raced the vblank IRQ), DMA internal-pointer
latch (must disable before re-arm, else source marches through EWRAM), stray
DMAs stopped on toggle-off. Flicker verdict from emulator testing: phase
alternation reads as biased blinking, not clean 30Hz -> single-phase (static
seams) is the default; flicker kept one-line-away for hardware A/B. Next:
Fit Height 180x160 (9/8 horizontal) - milder artifacts than both current
modes, correct aspect.

## Progress log (see src/scaling.c, src/scaling.s, lcd.s hooks)

- Phase 0 ✓ — clean toggle; `scaling_restore` = all-dirty rebuild (AfterLoadState
  recipe). Gotcha: asm called from C needs `global_func` (Thumb interworking).
- Phase 1 ✓ — pair cache + frame builder in C (scaling.c), verified on Catrap.
  Hard-won lessons, in order:
  - Cache is *converted pixel data* → tile-data writes must invalidate. Coarse
    full-rebuild-on-dirty overran vblank every frame on animated games (looked
    fine paused, garbage running). Fix: granular in-place reconvert of only the
    triples whose source tiles changed + dirty-row-only map updates.
  - Cache exhaustion across screen transitions → self-heal full reset (live
    screen always fits: 180 pairs < 255 slots).
  - 4bpp pixel 0 is transparent → GB color 0 fell through to backdrop
    ("inverted" look). Fix: pixel values 1..4 (+0x11111111 in converter),
    palette 8 entries 1..4 written directly from gbc_palette, black backdrop.
  - UI flag: read `ui_border_request` (live), not `ui_border_screen` (updated
    by add_ui_border, which the scaled path skips).
- Window layer ✓ — full-width case (WX<8), rows sourced from window map from
  WY>>3 down, same pair pipeline (Catrap status bar).
- Sprites ✓ (v6/v7) — display_sprites runs, then scaling_fix_oam rewrites OAM:
  affine double-size, 4 param sets for GB flip combos (affine has no flip
  bits), X = ceil(1.5*gbx)-2. PA=170 not 171: 171 drops an edge column
  (splits multi-sprite characters); 170 duplicates it and neighbors cover it.

## Direction change (2026-08-06): target modes

1.5x-wide-only is visually rejected: half of all columns doubled = fat uneven
pixels, and aspect gets 35% wider than a real GB. Artifact severity is
proportional to the fraction of doubled columns, so the target modes are:

| Mode | Size | Horizontal | Vertical | Notes |
|------|------|-----------|----------|-------|
| Off | 160×144 | 1:1 | 1:1 | authentic, 40/8 borders |
| **Fit Height** | 180×160 | **9/8** (1 in 8 doubled) | 10/9 line-repeat | aspect error only +1.2%, 30px side borders — the primary target |
| Full Stretch | 240×160 | 3/2 (current engine) | 10/9 line-repeat | Nintendo-style stretch, for people who hate borders |

Key techniques:
- **Vertical 144→160 is nearly free**: per-scanline BG0/BG1 VOFS table via
  HBlank DMA (PocketNES scale75 in reverse — repeat every 9th line instead of
  dropping every 4th), flicker-alternating which lines repeat for FRM
  blending. Composes with any horizontal mode. Sprites: affine PD=230
  (256*144/160), Y ×10/9.
- **Horizontal 9/8** = tile-domain like today but groups of 8 source tiles →
  9 output tiles (one doubled column per tile, rotating position). Cache keys
  become 8-tile groups (~76 visible groups × 9 tiles ≈ 22KB, fits). Sprites:
  affine PA=228 — near 1:1, much better looking than 1.5x.
- True filtering (FIR/Scale2x) is not feasible: needs dual-phase VRAM
  (~42KB, doesn't fit) or CPU we don't have. Flicker is the affordable blend.
- 178-wide "exact" fit is rejected: 89/80 coupling for an invisible 2px.

Work order: (1) vertical line-repeat stacked on current 1.5x = Full Stretch,
validates the technique + flicker on real FRM hardware cheaply; (2) 9/8
horizontal engine → Fit Height; (3) menu = Off / Fit Height / Full Stretch;
(4) polish (flicker tuning, CGB attrs, gamma, priority).

## Still open

- v8 scroll engine bugs seen in Balloon Kid (black BG areas, blinking) —
  shared foundation for all modes, debug carries over
- Multi-palette DMG colorization ignored (scaled = single palette; palette
  is map-entry-bits only, cache unaffected — read the SGB attribute source)
- CGB attributes (palette/flip/bank) in cache key — CGB games wrong colors
- Gamma not applied to scaled palette; GB "behind BG" sprite priority lost
- Sub-tile WY / partial-width window; SGB borders disabled by design
- Per-scanline SCX splits (games scrolling mid-frame) not supported

## Why the approach changed

The Mode 4 experiment is dead for three confirmed reasons:

1. **VRAM overlap (confirmed by experiment 2026-08-05).** A Mode 4 page-1 framebuffer
   spans 0x0600A000–0x06013600, trampling live emulator data: tile cache/SGB region
   (0x0600C200–0x0600EFFF) and the font/UI/BG-map region (0x0600F000+). Test: a
   5120-word fill ending exactly at 0x0600F000 survives toggle-back (glitched only);
   the full 9600-word fill crashes with PC in VRAM. The old "loop too slow" theory is
   disproven. There is no free 38KB anywhere in VRAM.
2. **CPU cost.** A software per-pixel re-render + 38KB copy per frame is several
   frames of CPU time.
3. **Renderer duplication.** Sprites, window, per-scanline effects would all need
   reimplementing.

New approach: stay in **Mode 0**, expand tiles **2 GBC tiles → 3 GBA tiles**
(1.5x horizontal NN) inside the existing tile pipeline, PocketNES-style philosophy:
do the work in the cheapest domain, use flicker to hide NN artifacts.

## Existing machinery we build on

| Piece | Location | Role |
|-------|----------|------|
| `render_tiles_vram` / `render_tiles_entry` | lcd.s:1016 | GBC 2bpp → GBA 4bpp tile conversion via CHR_DECODE, called from dirty/recent tile queues |
| `consume_recent_tiles`, `consume_dirty_tiles` | lcd.s:820–940 | incremental conversion of changed tiles at vblank |
| `display_bg` / `render_dirty_bg` | lcd.s:2413 | GBC tilemap → GBA screenblock conversion |
| `do_gba_hdma` + vcountfptr | lcd.s:2931 | per-scanline DMA0/1/2 feeding BG0CNT/DISPCNT/WIN0H from tables (same infra PocketNES's scale75 exploits) |
| `display_frame` | lcd.s:2361 | vblank hook, already branches on `g_scale_mode` |
| Menu toggle `changescale()` | ui.c | cycles `g_scale_mode`, already works |
| ClearDirtyTiles / all-dirty mechanisms | lcd.s:941 | full rebuild — our clean toggle-off restore |

VRAM layout today (from lcd.s:39 comment + render_tiles destinations):

| Range | Contents (normal mode) |
|-------|------------------------|
| 0x06000000–0x3FFF | sprite+shared tiles, banks 0/1 (char base 0) |
| 0x06004000–0x7FFF | solid tiles, border tiles, 4 maps |
| 0x06008000–0xBFFF | BG tiles banks 0/1 (char base 2) |
| 0x0600C000–0xCFFF | font + SGB/AGB data (SNES_VRAM 0xC220…) |
| 0x0600D000–0xFFFF | screenblocks 26–31: 6 GB maps + 2 UI maps + border map |
| 0x06010000–0x13FFF | (bitmap-mode-only OBJ area, unused in Mode 0) |
| 0x06014000–0x17FFF | OBJ tiles |

## The central design problem: pair coupling

1.5x NN of 16 pixels (2 source tiles A,B) → 24 pixels (3 output tiles X,Y,Z):

```
X = A0 A0 A1 A2 A2 A3 A4 A4     (pure A)
Y = A5 A6 A6 A7 B0 B0 B1 B2     (MIXED A+B)
Z = B2 B3 B4 B4 B5 B6 B6 B7     (pure B)
```

Tile Y depends on **both** source tiles. Therefore expanded output is keyed by the
**adjacent map pair**, not by a single tile number. Consequences:

- We cannot keep a fixed tileNumber→tileNumber mapping like `render_tiles_vram` does.
- We need a **pair cache**: key = (mapEntryA, mapEntryB) including tile ids, banks and
  flips; value = a triple of GBA tile slots. Repeated pairs (very common in game
  backgrounds) share slots.
- Pairs are anchored to **even source-map columns** (columns 0&1 → triple, 2&3 → …).
  This makes pairing map-relative and stable under scrolling: scroll never re-pairs
  tiles, only map edits do. Scrolling is handled purely by X-scroll (×1.5) on the
  output map.
- **Palette problem**: tile Y may mix two GBC palettes but a 4bpp map entry has one
  palette. Phase 1 compromise: Y uses A's palette (minor artifact when A,B palettes
  differ mid-pair — locally uniform art makes this rare). If artifacts prove bad,
  fallback plan is an 8bpp BG (palette baked into pixel values, 2× tile bytes).

## Geometry

- Source GBC map: 32 cols × 32 rows (256px wide) → scaled 384px wide → GBA **64×32
  map** (512px, 4KB screenblock ×2), 48 columns used, rest transparent.
- Output tile budget (worst case one map): 16 pairs × 32 rows = 512 distinct triples
  = 1536 tiles × 32B = 48KB. Doesn't fit worst-case; **cache with eviction**
  (on overflow: full re-mark dirty, same spirit as `recent_tiles_full`). Typical
  games use far fewer distinct pairs.
- Vertical: 144 lines fit natively (8px top/bottom borders). No vertical work in
  1.5x-wide mode.

## VRAM layout in scaled mode

When `g_scale_mode != 0`, normal BG charblocks/maps are idle (tile BG hidden, SGB
border disabled). Repurpose:

| Range | Scaled-mode use |
|-------|-----------------|
| 0x06004000–0xBFFF | pair-cache tile data: 32KB = 1024 4bpp tiles = 341 triples |
| two free screenblocks in 0x0600D000–0xFFFF (the 6 GB-map blocks; UI maps stay!) | 64×32 output map |
| 0x06000000–0x3FFF, 0x06014000+ | untouched: sprite tiles keep converting normally (affine OBJ reuses them as-is) |
| font + UI maps | untouched — menu overlay keeps working in scaled mode |

Exact screenblock numbers to be pinned down in Phase 1 by reading ui.c/lcd.s BG
register setup (must not collide with UI maps).

**Toggle-off restore**: mark all tiles + maps dirty (existing mechanisms) so the
normal renderer rebuilds everything it may have lost. This also retroactively fixes
the corruption seen in all experiments.

## Sprites

Hardware affine OBJ, zero extra tile VRAM:
- One affine parameter set: PA = 256×2/3 ≈ 171 (0xAB), PD = 256 (no vertical scale).
- Double-size affine rendering area so the 12px-wide result isn't clipped
  (8x8 → 16x16 area, 8x16 → 16x32).
- OAM X = round(gbcX × 1.5) − center adjustment; Y unchanged (+border offset).
- Hook wherever OAM entries are built today (display_sprites path).

## Scroll & per-scanline effects

- Static: BG X-scroll = round(scrollX × 1.5) + phase offset; Y-scroll unchanged.
- Per-scanline (games using mid-frame scrollX via the HDMA tables): the table
  builder gets a scaled variant — X entries ×1.5 — analogous to PocketNES's
  scale75 rewriting dma buffers. WIN0H becomes full-width 240.
- GBC window layer: window X ×1.5. The window is rendered via the second map path
  (bg0H/bg1…); it joins the pair-cache system in a later phase (initially: window
  unsupported in scaled mode → games with windows show them unscaled-wrong or
  hidden; decide in Phase 2 testing).

## Flicker (the PocketNES trick, adapted)

Goal: alternate NN phase per frame (`A A B` vs `A B B`) so FRM/LCD persistence blends
the doubled columns.

Naive per-frame reconversion of all tiles is too slow. Cheap version:
- Store **both phase variants** of each cached triple (VRAM cost ×2 → budget drops
  to ~170 triples; may be enough, needs measurement).
- Frame parity flips a single thing: either the char-base bit in BG0CNT (if the two
  variants live in separate 16KB-aligned blocks) or +offset added to map entries
  (worse). Preferred: variant blocks at 0x4000 and 0x8000, flip char base each
  vblank — one register write, exactly PocketNES's `twitch` spirit.

This is Phase 4; ship static NN first and evaluate whether artifacts warrant the
VRAM halving.

## Phases

**Phase 0 — cleanup & safety net**
- Remove Mode 4 test code from `scaling_render_frame` (keep menu/`g_scale_mode`).
- Delete Mode 4 branch in `display_frame`; scaled path becomes tile-based setup.
- Toggle-off: trigger full dirty rebuild (tiles + maps + palettes) → verify toggling
  on/off no longer glitches even with scaling doing nothing yet.
- Add `scaling.s` to all.s includes; strip its dead commentary into real code later.
- Milestone: toggle in menu is a clean no-op. **Testable in No$GBA.**

**Phase 1 — expansion core, static picture**
- Build the two 512-byte 2px→3px expansion tables (phase A now, phase B stored for
  later).
- Implement pair conversion: read 2 GBC tiles from XGB_VRAM, produce 3 GBA tiles
  via CHR_DECODE + expansion table (ARM asm in scaling.s, modeled on
  render_tiles_1_loop).
- Implement minimal pair cache + output map builder for the visible BG map (ignore
  scroll, window, sprites; assume scrollX=0 screens like the Catrap title).
- BG0CNT scaled-mode setup: 64×32 map, char base at 0x4000-region, other BGs hidden.
- Milestone: Catrap title screen displayed 240px wide, correct colors. Toggle back
  clean (Phase 0 net).

**Phase 2 — dynamics: map updates & scrolling**
- Hook GBC map writes (dirty-map path) → update pair cache + output map
  incrementally.
- X-scroll ×1.5 incl. sub-tile phase; per-scanline HDMA table scaled variant.
- Cache eviction policy + overflow full-rebuild.
- Window layer decision (support or suppress).
- Milestone: Catrap gameplay + a scrolling game (e.g. a platformer) fully playable
  wide.

**Phase 3 — sprites**
- Affine parameter setup, OAM transform (X ×1.5, double-size flag) in the
  display_sprites path when scaled.
- Milestone: sprites aligned with scaled BG in gameplay.

**Phase 4 — flicker blending**
- Dual-phase variant tiles + per-frame char-base flip; measure VRAM pressure on
  real games; keep behind its own toggle if borderline.

**Phase 5 (later, separate) — full-screen mode (240×160)**
- Vertical 144→160 = repeat every 9th line via per-scanline Y-scroll tables +
  twitch alternation + content-aware line choice (direct PocketNES scale75
  transplant, in reverse).

## Risks / open questions

- **Pair-cache worst case**: busy CGB games (many distinct pairs) may thrash the
  341-triple budget → visible wrong tiles or rebuild hitches. Mitigation: measure
  early with real games; 8bpp fallback halves budget further, so palette compromise
  is preferred.
- **Conversion bursts**: scene transitions dirty everything at once; may need the
  existing "N tiles per frame" throttling pattern from recent_tiles.
- **UI overlay while scaled**: menu draws on its own BG — verify its map/char
  locations don't collide with our repurposed blocks before finalizing layout.
- **SGB borders**: disabled in scaled mode by design (screen is full-width anyway).
- **Middle-tile palette compromise**: acceptability unknown until seen on hardware.

## Test protocol (per phase)

1. Build (make + font.o workaround), append Catrap → `test_catrap_scaling.gba`,
   copy `.elf` for symbols.
2. No$GBA: toggle scaling on → check picture; toggle off → check clean restore;
   enter/exit menu repeatedly; reset; load/save state.
3. mGBA for accuracy cross-check; real Omega DE hardware (FRM screen) for flicker
   phases.
