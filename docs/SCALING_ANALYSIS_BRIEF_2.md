# Analysis Brief 2: Jagoomba Color Scaling — GBC era (v34) — for LLM analysts

You are analyzing a Game Boy (DMG+GBC) emulator for GBA (jagoombacolor, a
Goomba Color descendant) and the tile-domain scaling engine we added. This is
an ANALYSIS task, not a coding task. Read code, reason about interactions,
produce a report. Challenge our hypotheses — we have candidate mechanisms for
most symptoms below, but we want them verified, refuted, or replaced with
better ones. Prior round: SCALING_ANALYSIS_BRIEF.md produced two reports that
found the v26 root causes; this is the second pass at a much more mature
engine.

**Deliverable:** `ANALYSIS_REPORT_2_<your-model-name>.md` in the repo root.
Structure: (1) verify/correct our architecture understanding; (2) root-cause
verdicts for S1-S5, ranked by confidence, each with exact code path and a
falsifiable prediction; (3) evaluate our proposed fixes vs your alternatives
(tradeoffs, hidden interactions); (4) prioritized plan. Cite files/lines.
Do not write code.

## 1. Current state

Branch `tile-scaling` of `jagoombacolor/`. Engine: `src/scaling.c` (~850
lines), `src/scaling.s` (restore), hooks in `src/lcd.s` (search
`g_scale_mode`), `src/ui.c` (`changescale`). Journal:
`docs/SCALING_TILE_PLAN.md` — READ IT FIRST; it records every fixed bug
(v14 to v34) so you do not rediscover them. Debug flag: `SCALING_DEBUG` in
`src/config.h` (colored letterbox cause-bars; Metroid palette for DMG).

What WORKS, forensically verified in mGBA (frame-diff vs unscaled at
matching state; see journal v29-v33 entries for numbers):
- Fit 9/8 h + 10/9 v geometry pixel-exact (settled scenes SAD 0.00/row;
  LADX dark room mean err 0.74/255).
- Full per-line scroll: sc_vquad[161][4] = {BG0HOFS,BG0VOFS,BG1HOFS,
  BG1VOFS} per scanline, streamed by DMA0 (count=4, dest-reload,
  HBlank-repeat), built each frame from the per-line capture the GB core
  already fills for the normal renderer (_bigbufferbase2, 6 words/GB line,
  xyscroll word at +2). Handles Batman's LY=16 HUD split AND its title's
  per-line SCY spin.
- GBC per-tile attributes (v33/v34): palette via map entry (GBA rows 0-7 =
  GBC BG palettes, entries 1-4, cell pixels 1..4); VRAM bank as tile id
  +384 in the cache key; h/v flips in the key (hflip = second nibble table
  sc_nibR, vflip = reversed row read). Wrong-art count in LADX forensics:
  zero.
- Cell cache: 688 VRAM cells, hash keyed bit28|a10|b10|flips4|phase4,
  mask-combine converter (~10 ops/row), VCOUNT time budget (work until
  scanout line 45), viewport-first ordering (v34), sweep wraps instead of
  restarting on mid-pass merges (v34), two-pass eviction age>16 then >8.
- OBJ: affine double-size sprites, PA/PD=227 (round-down-to-overlap; no
  seams), 4 param sets for flips; scaling_fix_oam converts GBA OAM
  in-place at vblank start after display_sprites.
- Window layer: BG1, tile-quantized top + WY&7 sub-tile seat in vquad,
  3-frame debounced state; WIN1 clips Fit to x=30..209.
- Toggle-off: deferred full rebuild at first emulated frame
  (g_restore_pending), verified pixel-identical restore for DMG; WAITCNT
  3,1+prefetch set at boot (was power-on 4,2!).

## 2. Emulator facts you need (verified in prior rounds)

- Normal-mode maps: screenblocks 10-15 (0x5000-0x7FFF), each entry written
  x3 (+0x800 color-zero, +0x1000 priority). Normal BG palette rows 8-15,
  written EVERY frame (gamma-adjusted) by transfer_palette_, which ALSO
  RUNS IN SCALED MODE (display_frame scaled branch, when
  bg_cache_updateok).
- UI font palette = GBA BG rows 5-6 (FONT_PALETTE_NUMBER 5,
  pocketnes_text.c loadfontpal copies 64 bytes). UI font tiles 0x4000.
- display_frame (and thus scaling_scaled_frame + transfer_palette_) runs
  EVERY vblank INCLUDING while the emulator UI menu is open over a paused
  game (proven: live scaled game visible behind the open menu).
- display_sprites writes GBA OAM from the GB OAM double-buffer each
  vblank — but check its early-out conditions (sgb_mask, gboamdirty?):
  we believe it can SKIP rewriting when GB OAM is unchanged.
- Dirty pipeline: DIRTY_TILE_BITS bit=tile/2, 768 tiles (bank1 bits at
  +24 bytes, so scaler tile ids 384+ align with it); dirty_map_words set
  by vram_W_9 for map-region writes in EITHER bank (attr writes included);
  vram_W_8 write-dedup byte VRAM_chr_lastAddr must be reset to 0xFF
  whenever bits are consumed. In scaled mode all three normal-renderer
  producers are gated off (v26).
- IWRAM is FULL (0x78C0/0x8000 + stack). The converter runs as Thumb from
  ROM (3,1+prefetch since v30). EWRAM tables hold sc_nib/sc_nibR/vquad.

## 3. Open symptoms (v34, tested in mGBA; LADX = Link's Awakening DX)

S1 LINK SPLIT: while walking, Link's two 8px OBJ halves intermittently
   render ~8-15px apart, snapping back on pose change. Our hypothesis:
   scaling_fix_oam converts OAM in-place and is NOT idempotent — if
   display_sprites early-outs on unchanged GB OAM, fix_oam re-scales
   already-scaled X (error grows with distance from x=30). VERIFY the
   early-out exists and the reapplication actually occurs; check ALSO the
   y path and affine param-set selection. Falsifiable prediction: split
   magnitude proportional to (x-30)/8, onset on pose-hold frames.

S2 FONT SHADOW: with scaling on, menu glyph shadows render tan (body
   near-white). v34 reloads fontpal at menu entry, but sc_load_palette
   (phase A, every scaled vblank — which still runs during the open menu)
   rewrites rows 0-7 including font rows 5-6 one frame later. Proposed
   fix: move scaled GBC palettes to rows 8-15 (unused while scaled; we
   already beat transfer_palette_'s same-frame writes by ordering —
   VERIFY that ordering holds on ALL paths incl. busy/nested frames and
   menu-open frames). Is suppressing transfer_palette_ in scaled mode
   safer? What else reads rows 8-15 or 0-7 that we would break (border?
   SGB? setdarkness/gamma paths)?

S3 STRADDLE COLOR BLEED: cells straddling two source tiles carry ONE
   palette (v34: majority-contributor tile). The minority sliver (1-4px
   at mid phases) renders in the wrong palette: blue-tinged jar edges,
   tinted braziers, chest grid discoloration. Proposed full fix: encode
   A-side pixels as values 1-4 and B-side as 5-8 (per-phase constant
   mask, +1 op/row), base palettes duplicated at 1-4/5-8 in rows 8-15,
   and a small LRU pool of "palette-pair rows" (A at 1-4, B at 5-8) for
   observed mixed pairs, selected via map entry. Keys stay palette-free.
   Evaluate: pool sizing (how many distinct straddle pairs per scene in
   real GBC games?), pair-row eviction vs map entries referencing them
   (staleness rules?), fonts (rows 5-6 reserved), DMG path, and whether a
   simpler trick (e.g. palette chosen per side of the duplicated column
   only) gets 90% of the benefit.

S4 SLUGGISH PERF (dynamic games): Batman ~8% slower under scroll load +
   burst stalls at transitions (red placeholder fill up to 55% for ~17
   frames); LADX title-entry garbage ~4.5s; menu slides 1.6x slower than
   native (Catrap). The budget is time-gated (to scanout line 45) and the
   converter is fast (mask-combine), so WHERE do the remaining cycles go?
   Candidates to weigh: sc_lookup hash probing under load, stamp loops
   (VRAM reads), per-frame vquad rebuild, per-frame full viewport
   re-lookup (~450 hash hits/frame), sc_load_palette, attr fetches, the
   wipe-block WIN_MAP clear, eviction passes, and the GB core's own
   share. Which optimizations actually matter next: IWRAM (what could be
   evicted from IWRAM to fit the converter?), per-row unchanged-map
   short-circuit, deadline tuning, lazy stamp rotation?

S5 WOBBLE on full-screen refreshes: when the whole screen re-renders
   (room transitions, big invalidations), motion shows hold-then-lurch
   (the periodic 4Hz stall trains were fixed in v32 by two-pass alloc,
   but bursts still hold frames then jump). Is the remaining wobble
   simply budget-less-than-one-screen-per-frame (fundamental; only faster
   conversion helps), or is there avoidable serialization (rows retried
   in unfortunate order, sweep/viewport interleaving, eviction storms
   during bursts)? Would a deterministic 2-frame split rebuild look
   better than the current convergence order? Any way to prioritize rows
   near the beam?

## 4. Specific analysis questions

Q1 display_sprites: enumerate ALL paths that skip the OAM rewrite while
   scaling_fix_oam still runs. Is an already-affine idempotency guard
   (attr0 bits) sufficient, or does any path write PARTIAL OAM (some
   entries fresh, some stale-converted)?
Q2 Palette ownership: produce the definitive table of who writes GBA BG
   palette rows 0-15 (and OBJ rows) in scaled mode, normal mode, UI open,
   setdarkness/gamma, SGB paths, borders. Then judge S2's rows-8-15 move
   and S3's pair pool against it.
Q3 Straddle-pair distribution: from LADX/typical GBC tilemaps, how many
   distinct (palA,palB) adjacent pairs occur per screen? (Static analysis
   of attribute-map locality is fine.) Does a 6-row LRU pool thrash?
Q4 Cycle budget: rough per-frame accounting of the v34 scaled path at
   steady state and during a 456-cell burst (ROM Thumb, 3,1+prefetch).
   Top three costs, and is the line-45 deadline the binding constraint on
   real hardware (vs mGBA)?
Q5 The mast-cutscene "drops to unscaled" (journal v33 segmentation): find
   the path that disables scaling there (LCDC-off handling?) — bug or
   intended fallback?
Q6 Room-entry OBJ palette lag (~23 frames of wrong sprite palettes in
   LADX): which path defers OBJ palette updates in scaled mode?

## 5. Solution directions to evaluate (tradeoffs; propose your own)

A. fix_oam idempotency guard (skip already-affine entries) vs converting
   from the GB OAM buffer into GBA OAM in one scaled-mode pass (replacing
   display_sprites). Which is more robust for partial-update paths and
   8x8-vs-8x16 modes?
B. Scaled palettes at rows 8-15 (optionally suppressing transfer_palette_
   in scaled mode) vs a remap table around the font rows. Interaction
   with toggle-off restore and the user gamma setting.
C. Palette-pair rows (S3 full fix) vs accept-the-fringe vs alternatives
   (per-column palette on the duplicated column, dithered sliver, 8bpp
   cells — VRAM cost?).
D. Perf: IWRAM tenancy review (what in .iwram.0-4 is cold enough to move
   out to fit a ~600B ARM converter?); per-row unchanged-map
   short-circuit; deadline tuning; better ideas welcome.
E. Wobble: deterministic 2-frame split rebuild vs convergence; beam-aware
   row ordering; brief mosaic/fade during big rebuilds.

## 6. Practical notes

- mGBA is the test proxy (hardware flashing is risky); it models
  waitstates and HBlank DMA accurately for our purposes.
- Frameskip-0 GIF captures exist for every symptom (ask if needed);
  forensic findings from prior rounds are summarized in the journal.
- Scaling Off must stay pixel-identical to the unmodified emulator, and
  DMG behavior must not regress (Catrap is the regression canary).
- Do not propose GBA affine BG modes (8bpp/tile-limit/per-line issues
  were evaluated and rejected in round 1) unless you have a genuinely
  new angle.
