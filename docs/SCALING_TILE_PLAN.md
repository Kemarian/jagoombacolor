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

## v16 diagnosis (2026-08-06, from user's catrap.gif frame-diffs)

Remaining artifacts (top-of-screen VOFS "waves", sprite tearing) are BOTH
vblank-overrun symptoms: heavy cache work inside the IRQ delays the next
frame's DMA re-arm and OAM update into scanout. NEXT STEP: frame-budgeted
conversion - cap cell conversions per vblank (~64), leave unfinished rows
invalid (sc_rowok=0) to complete over following frames; display state must
never depend on per-frame workload. Analyze future gifs with frameskip 1-2
(ffmpeg extract + PIL diff); frameskip 5 masks the high-frequency issues.

## Next steps (planned 2026-08-06, priority order)

1. **Autosleep OFF by default in jagoombacolor** (mirror PocketNES omega-tweaks:
   stime=3 + matching sleeptime; remember .sbss rejects nonzero initializers -
   move the var out of EWRAM_BSS if needed).
2. **Regression audit: scaling OFF must equal the user's current build.** Diff
   the tile-scaling branch against upstream for every non-scaling change and
   confirm the normal path is untouched when g_scale_mode==0. Must keep:
   per-game palette saving in SRAM config (sram.c/h + GFX_reset hook, with SGB
   auto-detection still used for new games) and GBA-enhanced mode default ON
   (gbz80.s request_gba_mode=1). New hooks must be no-ops when off: lcd.s
   branches fall through to original code, scaling.c never runs, exports and
   the all.s scaling.s include are inert. The baked build must not be inferior
   to the user's current kernel (palette fixes + gba-on).
3. **Bake into omega-de-kernel**: regenerate source/goomba.h from the new
   jagoombacolor.gba (recipe in BUILD.md), build kernel, name ezkernel.bin.
   Hardware flash only when 1-2 are verified (two-step update, brick risk).
4. **Frame-budgeted conversion** (the v16 diagnosis fix): cap cell conversions
   per vblank (~64), unfinished rows stay invalid and complete next frames -
   kills top-of-screen VOFS waves + sprite tearing from vblank overrun.
5. Then: sprite seam polish, multi-palette DMG colorization, CGB attrs.

## v20dbg profiler verdict (2026-08-07): the viewport walk is too heavy

Backdrop raster bars measured it: panning frames run RED to scanline ~69
and GREEN (OAM pass) to ~97; worst frames RED through all 160. The scaler
eats 40-100% of frame time - THE cause of slowdown + seam jitter + busy
cascades. Budget capped conversions but not the bookkeeping: a C0 change
rewrites all ~500 visible cells (lookups + VRAM-read stamps) every frame.

v21 restructure:
1. Ring viewport: slot = C mod 64 in a 64x32 map -> panning builds ONLY the
   entering column (~19 lookups/frame). Content wrap (period 36/48 vs 64)
   breaks adjacency only when the window spans the seam -> full rebuild
   accepted there (once per 288/384px scrolled).
2. Stamp rotation: protect visible cells by stamping 1/8 of rows per frame
   (evictor tolerates gen age 16) instead of ~500 VRAM reads every frame.
3. Guarantee-early display programming: registers/VOFS/DMA in the first
   lines of vblank ALWAYS; map/VRAM updates may trickle after (cells keep
   old content until replaced - mid-scan cell swap is a 1-line-late detail,
   not a torn frame).

## v26 synthesis (2026-08-08) - from ANALYSIS_REPORT_GPT-5.6-SOL + claude-fable-5

Consensus root cause (S1+S2+S3): the tile pipeline's PRODUCER half runs
unowned in scaled mode. newframe_vblank (GB line 144, non-IRQ, no mode
check) store_recent_tiles STEALS DIRTY_TILE_BITS (scaler invalidation
starved; only the overflow fallback ever fed it) and flush_recent_tiles
renders normal tiles into 0x8000-0xBFFF = live cell cache, mid-scanout.
S2 = stale RECENT_TILES replay after restore (consume_buffer still set;
restore's walk interleaves with the replay -> half-corrupted glyphs);
restore's VRAM coverage itself verified complete by both analysts.

Adopted unique finds: sentinel collision (C0==0 -> (u16)(C0-1)==0xFFFF
invalid marker -> misclassified pan); budget-exhausted dirty rows don't
persist invalidation; window wipe-on-change guarantees blank frames during
slides (fix: stale-until-replaced, like BG rows); S4 likely content not
timing (which line doubles moves with scrollY - A/B test); S5 = sprite
priority (window BG1 prio2 ties OBJ prio2, BG wins -> sprites sink behind
menu; remap normal sprites to prio1) + possibly per-line OBJ cycle budget
(double-size affine ~5x cost). Rejected: GPT's bank1-OBJ-skip claim (dirty
bits are address-based, both banks converted); GFX_reset-level restore.
Upstream bug noted: AfterLoadState clears vram_packets with outdated sizes.

v26 batch: P0 producer gating + restore queue-clears + sweep-restart +
sentinel/persist fixes; P1 wipe-kill; P2 sprite prio remap (+WIN1 clip
later). P3 (after retest): WIN0-split window model replacing debounce;
IWRAM ARM converter + EWRAM gen-shadow + timer budget; S4 A/B.

## v28 (2026-08-08) - THE DMA COUNT BUG + deferred restore + WIN1 clip

GIF forensics on v27 (two fable agents, 4 captures frameskip 0) measured:
window/text block vertically UNstretched (slope exactly 1.0, constant
offset), BG stretched only piecewise (89 consecutive rows at g=y-8, bands
with different phases), screen-fixed dead scanlines showing backdrop
(y=1,5,6,11,23,34...), status bar 15 rows up + garbage rows 152-159, game
logic 22% slow (timer 123fr/s vs 100, blink 32.8 vs 26.9), builder overrun
every enemy blink (~33fr), content bleeding 3px/9px into borders, menu
open 2.8x slower with 8px/17fr jerky window steps.

ROOT CAUSE (explains ALL of the above): HBlank VOFS DMA armed with
count=159. A repeat-HBlank DMA transfers its WHOLE count every hblank
(count reloads, source keeps advancing) - we sprayed 159 halfwords into
BG0VOFS/BG1VOFS per line, the source walked ~51KB of EWRAM per frame, and
each line's VOFS became whatever the walk ended on: piecewise-constant
slopes of 1.0 (constant EWRAM regions), dead lines (values pointing at
blank map rows), and a massive DMA bus tax (the 22% slowdown + overrun
sensitivity). Normal mode's own stream (count=6 = 6 regs/line, dest-
reload) was the miscopied template. Fix: count=1 at all 4 arm sites.

Also v28: (a) toggle-off restore DEFERRED to first newframe_vblank after
UI exit (g_restore_pending; changescale only stops DMAs) - v27's in-UI
restore still produced broken fonts despite verified-complete VRAM
coverage, so the rebuild now runs when nothing can follow it; IME masked
around the call. (b) WIN1 clips fit-mode content to x=30..209 (cells are
opaque 1..4; 24-cell viewport is wider than 180px), WINOUT=BG3 keeps the
border layer. Left as known: 8px window quantization (P3 WIN0-split),
sub-tile wy loss (<=7px), budget/overrun economics (P3 IWRAM converter).

## v29 (2026-08-08) - sub-tile window seating + OBJ seam fix

v28 forensics verdict (two fable agents, 4 GIFs): THE DMA FIX WORKED -
settled menu bit-exact (SAD 0.00/row, true 10/9 both layers, zero broken
glyphs), zero red content scanlines, WIN1 clip exact (x=30..209, 149/149),
menu open 2.8x -> 1.6x slower, OAM-at-vblank-start invisible (0 green).
Two defects remained, both now root-caused:

1. WINDOW TILE QUANTIZATION: wtop=WY>>3 floor-seats the window - ROUND
   bar (WY=135) 7 rows high with its tilemap row 1 (white body) filling
   rows 152-159 = "the white bar"; menu (WY=92) 4 rows high. v29: WY&7
   baked into sc_vtab1 (per-line VOFS absorbs the remainder); window now
   seats at ceil(WY*10/9) exactly. wsub is undebounced (<=7px lead for
   <=3 frames during slides; exact at rest).
2. OBJ SEAM: stacked 8px sprite units sit 9px apart after 10/9 rounding
   but PD=230 painted 8.90px/unit -> 1px background gap severing the cat
   (row 91 standing, 89 pushing; pose-dependent = "some positions").
   v29: PD=227 paints 9.02px (round-down-to-overlap, same rule as PA).

Also: dbg palette baked = Metroid (auto-detect fallback in GFX_reset, not
just the palettebank default - the GBC table fallback overwrote it) for
hue-separable layers in future captures. Known remaining: menu slide 8px
steps + 1.6x cadence (P3 WIN0-split + budget), open/close erase-draw
ordering (5f dropout / 2f doubling), feet 1-2px low (recheck after PD
change), speed unmeasured in v28 GIFs (5.03x timebase mismatch between
captures - re-capture with identical recorder settings).

## v0.6-k2 RELEASE (2026-08-08) - engine works; firmware baked

User confirmation on v29: "much, much better. cat split is gone, status
bar is in its place." The scaling engine's correctness core is DONE:
- true 10/9 vertical + 9/8 horizontal (Fit), verified bit-exact vs
  resampled reference at the settled menu (SAD 0.00 per row)
- window layer sub-tile-exact (WY&7 in vtab1), ROUND bar at rows 151-159
- affine sprites seam-free (PA=227/PD=227 round-down-to-overlap)
- WIN1 clip x=30..209 exact; no backdrop holes; no scatter/flash/tears
- toggle Off pixel-identical restore via deferred rebuild
The count=1 DMA lesson is the headline: one wrong halfword count cost
three debug versions (v26-v28) of chasing its downstream symptoms.

Release: jagoombacolor v0.6-k2 (debug bars removed, Metroid hardcode
removed, labels "Fit"/"Stretch") baked into omega-de-kernel as goomba.h;
kernel theme switched back to LIGHT (draw.h DARK undefined). Deliverable:
ezkernel_k2.bin (rename to ezkernel.bin on SD root to flash).

Still open (P3, polish): menu slide is tile-stepped (9px jumps, ~11f
cadence, 1.6x slower than native) -> WIN0-split window model + budget
tuning/IWRAM converter; open/close erase-draw ordering (5f bar dropout /
2f doubling); feet seat 1-2px low (recheck after PD change); speed
re-measure with identical recorder settings; CGB colorization of scaled
mode still unaddressed (DMG-only engine).

## v29 verification (2026-08-08, Metroid-palette captures)

Geometry DONE, measured exact: every window row at ceil(WY*10/9) with
delta 0 (bar text 152-158, menu panel top 103, SCROLL/RESTART/EXIT all
exact); settled menu a pixel-EXACT 9/8x10/9 resample (SAD=0, unique
phase); cat seam gone in all 51 visible frames, feet in exact floor
contact, height 26 rows = 23*256/227; ZERO wrong-hue patches, zero
in-band red, zero dead scanlines. SPEED RECOVERED: settled gameplay
matches orig (monster toggle 26 vs 27 frames, consistent timebases) -
v28's "5x" was capture-side, and the v27 22% tax is gone with the DMA
count fix.

Remaining (all one subsystem - window-layer transition pipelining):
displayed slides 1.64x slower with a 2-generation ~5px oscillation;
ghost bars 3-4f after the GB window is gone; 8f bar-return dropout;
5f stale lead-row text; NEW close-start overshoot ~5px/3f = undebounced
wsub leading debounced wtop on WY reversal (the documented trade-off,
now measured). All queued under the P3 WIN0-split window rework.
