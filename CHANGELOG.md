# Changelog — Kemarian fork (jagoombacolor)

Fork versioning: upstream version + `-kN` suffix, shown in the emulator menu
("Jagoomba v0.5-k1 on GBA"). The scaling engine additionally shows its own
build number in the Display Settings menu ("Fit vNN") — that number tracks
scaling iterations, `-kN` tracks fork releases.

## v0.5-k1 (2026-08-07) — branch `tile-scaling`

Features on top of upstream v0.5:
- **Per-game palette saving**: palette choice persists in SRAM config per
  game; SGB palette auto-detection still applies to games without a saved
  config.
- **GBA-enhanced mode ON by default** (`request_gba_mode=1`).
- **EZ Flash config persistence**: `using_flashcart()` returns 1.
- **Autosleep OFF by default** (menu cycle unchanged: OFF→5min→10min→30min).
- **Experimental scaling modes** (Display Settings → Scaling, engine v16):
  - *Fit* 180×160 — 9/8 horizontal + 10/9 vertical, correct GB aspect
    (±1.2%), 30px side borders.
  - *Stretch* 240×160 — 3/2 horizontal + 10/9 vertical, full screen.
  - Tile-domain cell cache in Mode 0, hardware-affine sprites, GB window
    layer on BG1, per-scanline VOFS line-repeat via HBlank DMA.
  - KNOWN ISSUES: vblank-overrun artifacts during heavy updates (top-line
    "waves", occasional sprite tearing) — frame-budgeted conversion planned;
    DMG multi-palette colorization shows single palette; CGB games not yet
    correct in scaled modes. Scaling OFF is bit-identical to the normal
    renderer path.

Design/journal: docs/SCALING_TILE_PLAN.md
