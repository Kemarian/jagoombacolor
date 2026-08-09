# Jagoomba Color - Kemarian fork (EZ Flash Omega DE)

A fork of [Jaga's Goomba Color](https://github.com/EvilJagaGenius/jagoombacolor)
tuned for daily use on the EZ Flash Omega Definitive Edition, plus an
experimental hardware-assisted scaling engine. Ships embedded in a matching
[omega-de-kernel](https://github.com/Kemarian/omega-de-kernel) build.

## Changed defaults vs stock Jagoomba

- **Per-game palette persistence** - the DMG palette you pick for a game is
  stored in the per-game SRAM config and restored on every launch (stock
  forgets it on restart).
- **Autosleep off by default** - no more surprise sleep mid-game; sleep is
  still available from the menu.
- **GBA enhanced mode requested by default**, and EZ Flash config
  persistence always enabled.
- ROM waitstates set to 3,1 + prefetch at boot (stock ran on the power-on
  4,2 no-prefetch default - everything is faster, scaling or not).

## Experimental scaling (Display Settings -> Scaling)

- **Fit**: 180x160 - 9/8 horizontal, 10/9 vertical, thin letterbox
- **Stretch**: 240x160 - 3/2 horizontal, 10/9 vertical, full screen

Sinnohman's fork (https://github.com/Sinnohman/jagoombacolor) pioneered
scaling for Jagoomba and inspired this work. The approach here differs
fundamentally: Sinnohman's renders the GB frame into a Mode 4 pixel
framebuffer in software every frame (simple and general, but the per-frame
pixel cost is heavy on real hardware). This fork never leaves the tile
domain: pre-stretched 8x8 cells are converted on demand from GB tile pairs
(cached and keyed by tile pair + sub-tile phase), the visible tilemap is
assembled from cache lookups, the 10/9 vertical stretch is a per-scanline
scroll stream (HBlank DMA), and sprites are hardware affine objects. A
static screen costs almost nothing and only rewritten tiles pay conversion,
so games run at full speed; mid-frame raster splits (Batman-style fixed
HUDs) are detected from the captured per-line scroll state and rendered as
independent bands. Current limits: DMG palettes only so far (GBC/SGB
per-tile attribute colorization is planned), and one raster split per frame.

Debug builds: set `SCALING_DEBUG` to 1 in both `src/config.h` and
`src/equates.h` to get colored letterbox timing/cause bars and a hardcoded
Metroid palette (hue-separable layers for frame-capture forensics).

---

# Original Jagoomba readme

Jaga's Goomba Color fork

A fork of Goomba Color with the goal of fixing bugs and incompatibilities in the original.  Based on the 2019-05-04 source.

Some notable hacks and games that have had issues fixed:
- Donkey Kong Land: New Colors Mode, https://www.romhacking.net/hacks/6076/ (file select menu accessible)
- Faceball 2000 (menu accessible)
- Kirby's Dream Land DX Service Repair, https://www.romhacking.net/hacks/6224/ (level 2 palette issues fixed)
- Konami GB Collections 2 and 4 (boots)
- Metal Gear Solid: Ghost Babel (elevator crash fixed)
- Pokemon Crystal (graphical corruption fixed)
- Wario Land DX, https://www.romhacking.net/hacks/6683/ (boots)

To build:
- Install the latest DevkitPro GBA tools
- Navigate Msys2 to this directory
- make
- Rename font.lz77.o to font.o and fontpal.bin.o to fontpal.o
- make

To test, I build a ROM with the resulting jagoombacolor.gba and the game I'm testing using goombafront.exe, then run it in mGBA.  You can find goombafront.exe as part of the Goomba Color releases.  For helpful debug symbols, take jagoombacolor.elf, put it in the same directory as the built ROM, and rename it to (ROM name).elf.  (Thanks to Endrift for the tip.)
Also included is a simple .bat file that will use gdb to dump debug symbols to a text file.

Thanks to:
- Dwedit for the Goomba Color emulator, which you can find at https://www.dwedit.org/gba/goombacolor.php.  If you'd like to incorporate my changes into Goomba Color, you're more than welcome to.
- FluBBa for the Goomba emulator before that: http://goomba.webpersona.com/
- Minucce for help with ASM and pointing me in the right direction.
- Sterophonick for code tweaks and featuring Jagoomba in the excellent Simple kernel for the EZ-Flash Omega carts: https://gbatemp.net/threads/new-theme-for-ez-flash-omega.520665/
- EZ-Flash for releasing the source to their modified Goomba Color builds, which hopefully allows this to support the Omega Definitive Edition's rumble features
- Nuvie for the code that saves the desired Game Boy type per game.
- Radimerry for the MGS:Ghost Babel elevator fix, Faceball menu fix, and SMLDX SRAM fix.
- Therealteamplayer for the default-to-grayscale code for GB games if no SGB palette is found.