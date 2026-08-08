# Analysis Brief: Jagoomba Color Tile-Domain Scaling — for LLM analysts

You are analyzing a Game Boy emulator for GBA (jagoombacolor, a Goomba Color
descendant) and a scaling engine we added to it. This is an ANALYSIS task, not
a coding task. Read code, reason about interactions, produce a report.

**Deliverable:** `ANALYSIS_REPORT_<your-model-name>.md` in the repo root.
Structure: (1) how the emulator's graphics pipeline actually works (verify our
understanding below, correct it where wrong); (2) root-cause hypotheses for
each open symptom, ranked by confidence, each with the exact code path and a
falsifiable prediction; (3) at least two architectural alternatives with
tradeoffs; (4) a prioritized fix plan. Cite files/lines. Do not write code.

## 1. Goal

Scale the GB screen (160×144) to use more of the GBA screen (240×160), on
real hardware (EZ Flash Omega DE, ARM7TDMI 16.78MHz), inside this emulator:
- Fit: 180×160 (9/8 horizontal NN, 10/9 vertical line-repeat), correct aspect
- Stretch: 240×160 (3/2 horizontal, same vertical)
- Off must be pixel-identical to the unmodified emulator, including after
  toggling scaled mode on and off mid-game.

Code: branch `tile-scaling` of jagoombacolor (in `jagoombacolor/`). The
engine: `src/scaling.c` (~650 lines), `src/scaling.s` (restore), hooks in
`src/lcd.s` (search `g_scale_mode`), menu in `src/ui.c` (`changescale`).
Design journal: `docs/SCALING_TILE_PLAN.md` (read it — it has the full bug
history so you don't rediscover fixed bugs).

## 2. Emulator graphics architecture (our understanding — VERIFY)

- Mode 0 tile renderer. GBC VRAM lives in `XGB_VRAM[0x4000]` (EWRAM); GBC
  tiles are converted to GBA 4bpp by `render_tiles_vram` (lcd.s ~1016) via
  CHR_DECODE, into charblocks: tiles 0x00-0x7F → 0x06000000 + OBJ 0x06014000;
  0x80-0xFF shared → 0x1000/0x9000/0x15000; 0x100-0x17F → 0x8000; bank1 +0x2000.
- Conversion is driven by dirty queues at vblank: `consume_recent_tiles`,
  `consume_dirty_tiles` (DIRTY_TILE_BITS set at write time in `vram_W_8`,
  bit = tile/2, plus a write-dedup byte patched into code at
  `VRAM_chr_lastAddr`). Maps: `dirty_map_words` (byte per map row, set in
  `vram_W_9`), consumed by `display_bg`/`render_dirty_bg` → six GBA maps at
  0x5000-0x7800 (TILEMAP1=10, TILEMAP2=13, each ×3 variants: normal, color-
  zero at +0x800, priority at +0x1000).
- Per-scanline register effects: vblank arms vcount IRQ chain
  (`force_ui_at_top` → `do_gba_hdma` at line 7): DMA0/1/2 in HBlank-repeat
  mode feed BG0CNT/DISPCNT/WIN0H per scanline from EWRAM tables
  (`bigbufferbase2` etc). This is how mid-frame LCDC/SCX/window tricks are
  emulated. `end_gba_hdma` stops them at line ~151.
- Sprites: `display_sprites` (lcd.s ~2784) writes GBA OAM directly each
  vblank from the GB OAM buffer (40 entries; hidden = attr0 0x02a0).
- UI overlay: font at 0x06004000 (charbase 1), UI map screenblock 9 (0x4800),
  shown via BG2/BG3 + windowing, `ui_border_request/screen` state.
- Palettes: `gbc_palette` → `gbc_palette2` → `transfer_palette` (with gamma)
  → GBA palettes 8-15 (BG) + OBJ palettes at 0x05000200.
- Vblank flow (lcd.s `vblankinterrupt`): canary check → `display_frame`
  (windows, add_ui_border, transfer_palette, display_bg) → display_sprites →
  consume_recent → consume_dirty → force_ui_at_top → arm vcountfptr → showfps.

## 3. Scaling engine architecture (v25, current)

When `g_scale_mode != 0`:
- lcd.s: `display_frame` branches to C `scaling_scaled_frame`
  (transfer_palette still runs); vblankinterrupt SKIPS consume_recent,
  consume_dirty, force_ui_at_top, do_gba_hdma arming; display_sprites still
  runs, followed by C `scaling_fix_oam`.
- scaling.c: generic N/D horizontal ratio. One output tile = cache cell
  keyed (tileA, tileB, phase); hash 2048 + up to 688 cell tiles at
  0x06006800-0xBFFF (charbase 1, indices 320-1007).
  BG0 = 64×32 ring map at 0x5000 (slot = column&63); window layer on BG1,
  32×32 map at 0x6000, screen-fixed. Vertical 10/9 via per-scanline VOFS
  tables fed by DMA0 (BG0) and DMA1 (BG1) in HBlank-repeat mode, re-armed
  (disable-first) every vblank. Sprites: affine, PA=227/170, PD=230, 4 param
  sets for flips, X/Y remapped in scaling_fix_oam.
- Safety systems accreted over v14-v25 (see journal): per-frame conversion
  budget (64) with retry-next-frame; resumable dirty sweeps (OBJ tiles get
  own budget 24); generation eviction (evict only if age>16) + stamp
  rotation (1/8 rows/frame) + stamp-what's-displayed rules; sc_busy IRQ
  reentry guard (busy path re-arms both DMAs only); 3-frame debounced
  window state (games move WX/WY mid-frame); per-frame snapshot of
  dirty_map_words (BG and window may share a tilemap).
- Toggle-off: `scaling_restore` (scaling.s) = set DIRTY_TILE_BITS and
  dirty_map_words all-1s, call render_dirty_tiles + render_dirty_bg; plus
  ui.c stops DMA0-2. Toggle-on: synchronous unbudgeted first build.
- Debug: backdrop = red during builder, green during OAM pass (raster bars).

## 4. Open symptoms (after v25, tested in mGBA on Catrap, DMG game)

S1. Game-menu (window) state still dynamically glitchy: text/tiles flicker
    or show wrong content during and after the window slide; behavior
    changed across v22-v25 fixes but never became clean.
S2. **Corruption persists in UNSCALED mode after toggling scaling off:**
    game font glyphs and sprites visibly damaged (half-corrupted letters)
    and it does NOT fully heal. scaling_restore's all-dirty rebuild is
    insufficient — something it doesn't cover gets corrupted. Candidates to
    examine: OBJ tile VRAM (our sc_convert_obj writes 0x06014000-0x17FFF
    including bank1 region 0x16000 — verify against what display uses and
    what render_dirty_tiles rebuilds), the shared-tile triple-destination
    scheme (0x1000/0x9000/0x15000), VRAM_chr_lastAddr patched byte state,
    consume flags (`consume_dirty`), recent-tiles ring state
    (RECENT_TILENUM/RECENT_TILES), vram_packets_* queues accumulating
    unconsumed during scaled mode and replaying garbage after toggle-off.
S3. Residual slowness during heavy churn (menu transitions) even though
    steady state is fast (raster bars sub-scanline when static).
S4. Seam-line micro-blink at vertical repeat lines (rows where source line
    repeats) under some conditions — was diagnosed as DMA re-arm timing
    jitter; verify whether the current arm-early ordering really bounds it.
S5. Sprites intermittently absent in game-menu state in scaled mode while
    present in unscaled (game hides only some sprites during pause).

## 5. Specific analysis questions

Q1. Enumerate EVERYTHING the normal renderer writes/depends on in VRAM and
    engine state that scaled mode perturbs, versus what scaling_restore
    rebuilds. Produce the exact "not restored" list (S2 root cause).
Q2. While scaled, GB writes still enqueue into vram_packets_* and
    recent-tiles machinery (we only skip the consumers). What happens to
    those queues over minutes of scaled play, and what do they do on
    toggle-off? Is there overflow/corruption/stale-replay?
Q3. Catrap uses mid-frame window tricks. Characterize what per-scanline
    behavior the normal renderer produces for Catrap's pause menu (via the
    hdma tables) and whether ANY single-sample-per-frame window model can
    render it acceptably; if not, what's the minimal per-scanline support?
Q4. Audit the interaction matrix of budget / eviction+stamps / debounce /
    row-validity (sc_row_c0/sc_wbuilt) for livelocks or starvation: cases
    where convergence never completes or oscillates (S1).
Q5. Is running all of this as C in ROM inside the vblank IRQ fundamentally
    viable on hardware (waitstates!), or should the heavy path move to
    IWRAM/asm or out of IRQ context? Estimate real cycle costs.
Q6. display_sprites + scaling_fix_oam interaction: find conditions where
    transformed OAM entries become invisible/wrong (S5) — e.g. Y wrap,
    double-size clipping at screen edges, hidden-slot detection.

## 6. Solution directions to evaluate (give tradeoffs; propose your own too)

A. Fix-forward: patch the enumerated holes (esp. complete restore: consider
   toggle-off calling the emulator's own full reinit path (`GFX_reset`-level
   or AfterLoadState-complete) instead of our partial recipe).
B. Deeper integration: instead of a parallel cache+map system, hook the
   EXISTING conversion/consume pipeline (render_tiles_vram, display_bg) to
   emit scaled output — one dirty system, no aliasing, restore trivial.
C. Keep normal renderer running unmodified underneath; scaled mode as pure
   presentation layer over its outputs (read GBA-side tiles/maps it
   maintains, convert incrementally) — normal mode never perturbed.
D. Constrain scope: scaled mode only for games without mid-frame tricks
   (detect and auto-fallback), ship Fit for the well-behaved majority.
E. Your own alternatives, including "the approach is not salvageable
   because X" if the evidence supports it.

## 7. Practical notes

- Test game: Catrap (DMG). mGBA + interframe blending ≈ hardware FRM proxy.
  GIF frame-diffing (ffmpeg extract + PIL) is our measurement method.
- Everything must hold on ARM7TDMI timing, ROM code has 3-waitstate 16-bit
  bus; vblank ≈ 83k cycles minus emulator's own work.
- Fixed-bug history in docs/SCALING_TILE_PLAN.md — do not re-report those.
- The unscaled path with scaling never enabled is correct by construction
  (audited); only post-toggle state is suspect.
