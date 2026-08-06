#ifndef __SCALING_H__
#define __SCALING_H__

// Tile-domain scaling - see spec/SCALING_TILE_PLAN.md

// Scaling modes for the Display Settings menu
#define SCALE_1X        0   // Native 160x144 centered (40px borders)
#define SCALE_FIT       1   // 180x160: 9/8 horizontal + 10/9 vertical
#define SCALE_FULL      2   // 240x160: 3/2 horizontal + 10/9 vertical
#define SCALE_MODES     3   // Total number of modes

// Current scale mode (defined in ui.c, saved with per-game config later)
extern u8 g_scale_mode;

// Full rebuild of tile data + BG maps (scaling.s). Call when scaling is
// switched off so the normal renderer regenerates anything the scaled
// mode clobbered in VRAM.
void scaling_restore(void);

// Reset the pair cache; call when scaling is switched on (scaling.c).
void scaling_enter(void);

// Per-frame scaled render: pair cache + map + display registers (scaling.c).
// Called from display_frame (lcd.s) at vblank when g_scale_mode != 0.
void scaling_scaled_frame(void);

// Rewrite OAM as 1.5x-wide affine sprites; called after display_sprites
// from vblankinterrupt (lcd.s) when scaled.
void scaling_fix_oam(void);

#endif
