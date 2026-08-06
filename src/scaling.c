// scaling.c - tile-domain 1.5x scaling: pair cache + scaled frame builder.
// Design: spec/SCALING_TILE_PLAN.md
//
// 2 GBC tiles (16px) expand to 3 GBA tiles (24px) with nearest-neighbor 1.5x.
// The middle output tile mixes pixels from both sources, so converted tiles
// are cached per *adjacent map pair*, keyed on the two effective tile numbers.
//
// v10 = reconstructed v9: scroll support + SCALE_FULL (vertical 10/9 via
// HBlank-DMA VOFS tables). DMG games, no CGB attributes yet.
//
// Geometry: the BG0 64x32 map is a ring of 16 pair-slots x 32 rows covering
// the full GBC map (slot == source pair index, row == source row), so
// hardware scroll wraps correctly. Only cells visible this frame are built,
// tracked with per-row built bits; dirty flags (set at write time by the
// vram_W handlers) clear them. Cache-full evicts triples not referenced
// this frame. The GB window layer is screen-fixed, so it lives on BG1 with
// its own map.
//
// VRAM in scaled mode (all rebuilt by scaling_restore on toggle-off):
//   0x06005000-0x5FFF  BG0 scaled map, 64x32 (screenblocks 10+11)
//   0x06006000-0x67FF  BG1 window map, 32x32 (screenblock 12)
//   0x06006800-0xBFFF  pair-cache tiles (charbase 1, indices 320..1021)
//   0x06004000-0x4FFF  UNTOUCHED: UI font + UI map (menu stays usable)
//   0x06014000-0x17FFF OBJ tiles - kept fresh by our dirty pass (the normal
//                      consume path is skipped while scaled)

#include "includes.h"
#include "asmcalls.h"
#include "scaling.h"

extern u8 lcdctrl0frame_;      // LCDC (FF40) latched at frame start (lcd.s)
extern u8 scrollX;             // SCX (FF43)
extern u8 scrollY;             // SCY (FF42)
extern u8 windowX;             // WX (FF4B), visual left = WX-7
extern u8 windowY;             // WY (FF4A)
extern u32 ui_border_request;  // bit0 = UI wants to be visible (lcd.s).
                               // NOT ui_border_screen: that copy is updated
                               // by add_ui_border, skipped while scaled.
extern u32 DIRTY_TILE_BITS[];  // set at VRAM-write time, bit = tile/2
extern u8 VRAM_chr_lastAddr;   // write-dedup byte in vram_W (0xFF = none)

#define SCALED_MAP       ((vu16*)0x06005000)
#define WIN_MAP          ((vu16*)0x06006000)
#define CHARBASE1        ((u8*)0x06004000)
#define OBJ_TILES        ((u8*)0x06014000)
#define CACHE_TILE_BASE  320                 // first tile index we own
#define MAX_TRIPLES      233                 // tiles 320..1018
#define BLANK_TILE       1021                // zeroed tile for empty map area
#define BG_PAL           8                   // GBC BG palette 0 lives here

#define VIS_ROWS         19                  // 144px + sub-tile scroll slack
#define VIS_PAIRS        11                  // 240px + sub-pair scroll slack

#define HASH_SIZE        1024
#define KEY_TOMB         1                   // tombstone (real keys have bit20)

EWRAM_BSS static u32 sc_hkey[HASH_SIZE];     // 0 = empty
EWRAM_BSS static u16 sc_htrip[HASH_SIZE];
EWRAM_BSS static u32 sc_trip_key[MAX_TRIPLES];
EWRAM_BSS static u8  sc_trip_gen[MAX_TRIPLES];
EWRAM_BSS static u32 sc_nib[256];            // GB plane byte -> 8 GBA nibbles
EWRAM_BSS static u16 sc_exp[256];            // 2 GBA pixels -> 3 (NN phase A)
EWRAM_BSS static u16 sc_built[32];           // per BG map row: pair-slot bits
EWRAM_BSS static u16 sc_wbuilt[18];          // per window screen row
EWRAM_BSS static u32 sc_dirty_snap[12];

// Vertical 144->160 (SCALE_FULL): per-scanline VOFS via HBlank DMA.
// delta[y] = y - source_line(y); two phases repeat different lines so an
// FRM/LCD screen blends the doubled lines (PocketNES-style flicker).
EWRAM_BSS static u8  sc_vdelta[2][161];
EWRAM_BSS static u16 sc_vtab0[161];          // BG0 VOFS per line (scrolled)
EWRAM_BSS static u16 sc_vtab1[161];          // BG1 VOFS per line (window)

static u16 sc_ntriples;
static u16 sc_tombs;
static u16 sc_evict_ptr;
static u8 sc_gen;
static u8 sc_tables_ok;
static u8 sc_active;
static u8 sc_last_lcdc;
static u8 sc_last_wy;
static u8 sc_flick;        // frame parity for vertical flicker phase
static volatile u8 sc_busy; // reentrancy guard: the menu toggle calls
                            // scaling_scaled_frame synchronously with IRQs
                            // on; if vblank fires mid-build, the IRQ call
                            // must skip, not interleave on half-built state

static void sc_build_tables(void)
{
	int b,i;
	for(b=0;b<256;b++)
	{
		u32 v=0;
		for(i=0;i<8;i++)
			v |= ((u32)((b>>(7-i))&1))<<(i*4);  // GB bit7 = leftmost = low nibble
		sc_nib[b]=v;
		// input byte = 2 pixels {p1:4,p0:4}; output = p0 p0 p1 (12 bits)
		sc_exp[b] = (b&0xF) | ((b&0xF)<<4) | ((b>>4)<<8);
	}
	// vertical delta tables: source_line(y) = (9y+phase)/10; the phase
	// offset moves which lines repeat so alternate frames blend on FRM
	for(b=0;b<161;b++)
	{
		sc_vdelta[0][b] = b - (9*b)/10;
		sc_vdelta[1][b] = b - (9*b+5)/10;
	}
}

// Convert one pair of GBC tiles to 3 GBA 4bpp tiles at dst (24 words).
// Pixel values are 1..4 (0 would be transparent in a 4bpp text BG; the
// normal renderer solves this with a color-zero layer we don't have).
static void sc_convert(const u8 *ta,const u8 *tb,u32 *dst)
{
	int r;
	for(r=0;r<8;r++)
	{
		u32 ra = sc_nib[ta[r*2]] | (sc_nib[ta[r*2+1]]<<1);
		u32 rb = sc_nib[tb[r*2]] | (sc_nib[tb[r*2+1]]<<1);
		u32 e0=sc_exp[ra&0xFF], e1=sc_exp[(ra>>8)&0xFF];
		u32 e2=sc_exp[(ra>>16)&0xFF], e3=sc_exp[ra>>24];
		u32 f0=sc_exp[rb&0xFF], f1=sc_exp[(rb>>8)&0xFF];
		u32 f2=sc_exp[(rb>>16)&0xFF], f3=sc_exp[rb>>24];
		dst[r]    = (e0 | (e1<<12) | (e2<<24)) + 0x11111111;
		dst[8+r]  = ((e2>>8) | (e3<<4) | ((f0 | ((f1&0xF)<<12))<<16)) + 0x11111111;
		dst[16+r] = ((f1>>4) | (f2<<8) | (f3<<20)) + 0x11111111;
	}
}

// 1:1 convert one GBC tile to a GBA 4bpp OBJ tile (pixel 0 stays
// transparent - correct for sprites).
static void sc_convert_obj(const u8 *src,u32 *dst)
{
	int r;
	for(r=0;r<8;r++)
		dst[r] = sc_nib[src[r*2]] | (sc_nib[src[r*2+1]]<<1);
}

static u32* sc_trip_vram(u32 trip)
{
	return (u32*)(CHARBASE1 + (CACHE_TILE_BASE + trip*3)*32);
}

static void sc_full_reset(void)
{
	int i;
	u32 *bt;
	u16 blank;
	vu16 *m;

	for(i=0;i<HASH_SIZE;i++) sc_hkey[i]=0;
	for(i=0;i<32;i++) sc_built[i]=0;
	for(i=0;i<18;i++) sc_wbuilt[i]=0;
	sc_ntriples=0;
	sc_tombs=0;
	sc_evict_ptr=0;

	bt=(u32*)(CHARBASE1 + BLANK_TILE*32);
	for(i=0;i<8;i++) bt[i]=0;

	blank = BLANK_TILE | (BG_PAL<<12);
	m=SCALED_MAP;
	for(i=0;i<0x800;i++) m[i]=blank;
	m=WIN_MAP;
	for(i=0;i<0x400;i++) m[i]=blank;
}

// Rebuild the hash from live triples when tombstones clog probing.
static void sc_rehash(void)
{
	int i;
	for(i=0;i<HASH_SIZE;i++) sc_hkey[i]=0;
	sc_tombs=0;
	for(i=0;i<sc_ntriples;i++)
	{
		u32 key=sc_trip_key[i];
		u32 h=(key*0x9E37u >> 6)&(HASH_SIZE-1);
		while(sc_hkey[h]) h=(h+1)&(HASH_SIZE-1);
		sc_hkey[h]=key;
		sc_htrip[h]=i;
	}
}

// Delete a key from the hash (leaves a tombstone).
static void sc_hash_del(u32 key)
{
	u32 h=(key*0x9E37u >> 6)&(HASH_SIZE-1);
	while(sc_hkey[h])
	{
		if(sc_hkey[h]==key) { sc_hkey[h]=KEY_TOMB; sc_tombs++; return; }
		h=(h+1)&(HASH_SIZE-1);
	}
}

// Find a triple slot: reuse a free one, or evict one not used this frame.
static u32 sc_alloc_trip(void)
{
	int scanned;
	if(sc_ntriples<MAX_TRIPLES)
		return sc_ntriples++;
	for(scanned=0;scanned<MAX_TRIPLES;scanned++)
	{
		u32 c=sc_evict_ptr;
		sc_evict_ptr = (sc_evict_ptr+1)==MAX_TRIPLES ? 0 : sc_evict_ptr+1;
		if(sc_trip_gen[c]!=sc_gen)
		{
			sc_hash_del(sc_trip_key[c]);
			return c;
		}
	}
	return MAX_TRIPLES;
}

// Look up (or convert) the triple for effective tiles a,b. Returns first
// GBA tile index of the triple, or BLANK_TILE if the cache can't hold it.
static u32 sc_lookup(u32 a,u32 b)
{
	u32 key = (a<<10) | b | (1u<<20);
	u32 h = (key*0x9E37u >> 6) & (HASH_SIZE-1);
	u32 first_free = HASH_SIZE;
	u32 trip;
	for(;;)
	{
		u32 k=sc_hkey[h];
		if(k==key)
		{
			trip=sc_htrip[h];
			sc_trip_gen[trip]=sc_gen;
			return CACHE_TILE_BASE + trip*3;
		}
		if(k==KEY_TOMB)
		{
			if(first_free==HASH_SIZE) first_free=h;
		}
		else if(k==0)
		{
			if(first_free==HASH_SIZE) first_free=h;
			break;
		}
		h=(h+1)&(HASH_SIZE-1);
	}
	trip=sc_alloc_trip();
	if(trip>=MAX_TRIPLES)
		return BLANK_TILE;
	sc_convert(XGB_VRAM + a*16, XGB_VRAM + b*16, sc_trip_vram(trip));
	sc_trip_key[trip]=key;
	sc_trip_gen[trip]=sc_gen;
	if(sc_hkey[first_free]==KEY_TOMB) sc_tombs--;
	sc_hkey[first_free]=key;
	sc_htrip[first_free]=trip;
	if(sc_tombs>HASH_SIZE/2) sc_rehash();
	return CACHE_TILE_BASE + trip*3;
}

void scaling_enter(void)
{
	sc_active=0;   // next scaled frame does a full reset
}

#define TILE_DIRTY(t) ((sc_dirty_snap[(t)>>6] >> (((t)>>1)&31)) & 1)

// Tile-data writes: reconvert affected cached triples in place (map and
// hash untouched), and refresh the OBJ copies of tiles 0..255 for both
// banks (the normal consume path that maintains OBJ VRAM is skipped while
// scaled, and games stream sprite tiles constantly).
static void sc_update_dirty(void)
{
	int i;
	u32 acc=0;
	for(i=0;i<12;i++)
	{
		sc_dirty_snap[i]=DIRTY_TILE_BITS[i];
		acc|=sc_dirty_snap[i];
		DIRTY_TILE_BITS[i]=0;
	}
	if(!acc)
		return;
	VRAM_chr_lastAddr=0xFF;   // reset write-dedup so new writes re-mark

	// OBJ tiles: each dirty bit covers a tile pair; sprites use tiles 0..255
	{
		u32 t;
		for(t=0;t<256;t+=2)
		{
			if(!TILE_DIRTY(t)) continue;
			// bank 0
			sc_convert_obj(XGB_VRAM + t*16,       (u32*)(OBJ_TILES + t*32));
			sc_convert_obj(XGB_VRAM + (t+1)*16,   (u32*)(OBJ_TILES + (t+1)*32));
			// bank 1 (CGB; zeros for DMG, harmless)
			sc_convert_obj(XGB_VRAM + 0x2000 + t*16,     (u32*)(OBJ_TILES + 0x2000 + t*32));
			sc_convert_obj(XGB_VRAM + 0x2000 + (t+1)*16, (u32*)(OBJ_TILES + 0x2000 + (t+1)*32));
		}
	}

	// BG pair cache: in-place reconvert
	for(i=0;i<HASH_SIZE;i++)
	{
		u32 key=sc_hkey[i];
		u32 a,b;
		if(key==0 || key==KEY_TOMB) continue;
		a=(key>>10)&0x3FF;
		b=key&0x3FF;
		if(TILE_DIRTY(a) || TILE_DIRTY(b))
			sc_convert(XGB_VRAM + a*16, XGB_VRAM + b*16,
				sc_trip_vram(sc_htrip[i]));
	}
}

// Build one map cell (3 entries) from a source map row at pair column pc.
// out = start of the destination map row (32-entry block-0 row).
static void sc_build_cell(const u8 *mrow,int pc,vu16 *out,int mode8000)
{
	u32 a = mrow[pc*2];
	u32 b = mrow[pc*2+1];
	u32 t0;
	u16 e;
	int x,k;
	if(!mode8000)
	{
		if(a<128) a+=256;   // 0x9000 region tiles
		if(b<128) b+=256;
	}
	t0 = sc_lookup(a,b);
	for(k=0;k<3;k++)
	{
		e = (u16)((t0==BLANK_TILE ? BLANK_TILE : t0+k) | (BG_PAL<<12));
		x = pc*3+k;
		// 64-wide map: columns 32..47 live in screenblock +0x400 halfwords
		if(x<32) out[x]=e; else out[x+0x400-32]=e;
	}
}

// Re-stamp the triple a built cell references so eviction skips it.
static void sc_stamp_cell(vu16 *out,int pc)
{
	int x=pc*3;
	u32 t = (x<32 ? out[x] : out[x+0x400-32]) & 0x3FF;
	if(t>=CACHE_TILE_BASE && t<CACHE_TILE_BASE+MAX_TRIPLES*3)
		sc_trip_gen[(t-CACHE_TILE_BASE)/3]=sc_gen;
}

// Main per-frame entry, called from display_frame (vblank) when scaled.
void scaling_scaled_frame(void)
{
	u8 lcdc;
	const u8 *gbmap;
	int mode8000,r,j;
	u16 dispcnt;
	int scy,scx,row0,fp;
	int wenable,wtop;
	const u8 *wmap;
	u8 *wdirty,*mdirty;

	if(sc_busy) return;   // vblank hit during the synchronous menu build
	sc_busy=1;

	if(!sc_tables_ok) { sc_build_tables(); sc_tables_ok=1; }
	if(sc_active)
		sc_update_dirty();
	if(!sc_active)
	{
		int i;
		sc_full_reset();
		sc_active=1;
		for(i=0;i<12;i++) DIRTY_TILE_BITS[i]=0;
		VRAM_chr_lastAddr=0xFF;
	}
	sc_gen++;

	lcdc = lcdctrl0frame_;
	scy = scrollY;
	scx = scrollX;
	gbmap = XGB_VRAM + ((lcdc&0x08) ? 0x1C00 : 0x1800);
	mode8000 = lcdc&0x10;
	mdirty = dirty_map_words + ((lcdc&0x08) ? 32 : 0);

	// GB window layer (screen-fixed, on BG1): common full-width case.
	wenable = (lcdc&0x20) && windowY<144 && windowX<8;
	wtop = windowY>>3;
	wmap = XGB_VRAM + ((lcdc&0x40) ? 0x1C00 : 0x1800);
	wdirty = dirty_map_words + ((lcdc&0x40) ? 32 : 0);
	if(((lcdc ^ sc_last_lcdc) & 0x78) || windowY != sc_last_wy)
	{
		int i;
		vu16 *m=WIN_MAP;
		u16 blank=BLANK_TILE|(BG_PAL<<12);
		for(i=0;i<0x400;i++) m[i]=blank;
		for(i=0;i<18;i++) sc_wbuilt[i]=0;
		// map-select/mode changes also invalidate BG rows
		if((lcdc ^ sc_last_lcdc) & 0x18)
			for(i=0;i<32;i++) sc_built[i]=0;
	}
	sc_last_lcdc=lcdc;
	sc_last_wy=windowY;

	{	// re-blank the blank tile every frame (cheap corruption insurance)
		u32 *bt=(u32*)(CHARBASE1 + BLANK_TILE*32);
		int i;
		for(i=0;i<8;i++) bt[i]=0;
	}

	// ---- BG0: visible window of the pair-slot ring ----
	row0 = scy>>3;
	fp = (scx>>4);
	for(r=0;r<VIS_ROWS;r++)
	{
		int mr=(row0+r)&31;
		const u8 *mrow;
		vu16 *out;
		// rows fully covered by the window don't need BG cells
		if(wenable && r>wtop+1) continue;
		if(mdirty[mr]) { mdirty[mr]=0; sc_built[mr]=0; }
		mrow = gbmap + mr*32;
		out = SCALED_MAP + mr*32;
		for(j=0;j<VIS_PAIRS;j++)
		{
			int pc=(fp+j)&15;
			if(sc_built[mr] & (1<<pc))
				sc_stamp_cell(out,pc);
			else
			{
				sc_build_cell(mrow,pc,out,mode8000);
				sc_built[mr] |= 1<<pc;
			}
		}
	}

	// ---- BG1: window layer rows (screen-space, no scroll) ----
	if(wenable)
	{
		for(r=wtop;r<18;r++)
		{
			int wr=r-wtop;
			const u8 *mrow = wmap + wr*32;
			vu16 *out = WIN_MAP + r*32;
			if(wdirty[wr]) { wdirty[wr]=0; sc_wbuilt[r]=0; }
			for(j=0;j<10;j++)
			{
				if(sc_wbuilt[r] & (1<<j))
					sc_stamp_cell(out,j);
				else
				{
					sc_build_cell(mrow,j,out,mode8000);
					sc_wbuilt[r] |= 1<<j;
				}
			}
		}
	}

	// Palette: our tiles use pixel values 1..4 in GBA palette 8.
	{
		vu16 *p = (vu16*)0x05000100;   // BG palette 8
		u16 *src = (u16*)gbc_palette;  // BG pal 0, colors 0..3
		p[1]=src[0]; p[2]=src[1]; p[3]=src[2]; p[4]=src[3];
		*(vu16*)0x05000000 = 0;        // backdrop = black
	}

	// BG0: prio 3, charbase 1, screenbase 10, 64x32
	REG_BG0CNT = 3 | (1<<2) | (10<<8) | (1<<14);
	// HOFS: pair-slot ring position + sub-pair phase (24 out px per slot)
	*(vu16*)0x4000010 = (u16)((fp&15)*24 + ((3*(scx&15))>>1));

	if(g_scale_mode==SCALE_FULL)
	{	// vertical 144->160: per-line VOFS tables for this frame's
		// flicker phase; line 0 set here, lines 1..159 via HBlank DMA
		const u8 *d = sc_vdelta[sc_flick];
		int y;
		for(y=0;y<161;y++)
		{
			sc_vtab0[y] = (u16)((scy - d[y]) & 0xFF);
			sc_vtab1[y] = (u16)((0   - d[y]) & 0xFF);
		}
		*(vu16*)0x4000012 = sc_vtab0[0];
		sc_flick ^= 1;
	}
	else
		*(vu16*)0x4000012 = (u16)((scy-8) & 0xFF);

	dispcnt = 0x1140;                          // Mode 0 + BG0 + OBJ + 1D obj
	if(wenable)
	{
		*(vu16*)0x400000A = 2 | (1<<2) | (12<<8);  // BG1: prio 2, char 1, map 12
		*(vu16*)0x4000014 = 0;                     // BG1HOFS
		*(vu16*)0x4000016 = (g_scale_mode==SCALE_FULL)
			? sc_vtab1[0] : ((u16)(-8) & 0xFF);    // BG1VOFS
		dispcnt |= 0x0200;
	}
	if(ui_border_request & 1)
	{
		*(vu16*)0x400000E = (9<<8) | (1<<2);   // BG3: UI map 9, charbase 1, prio 0
		*(vu16*)0x400001C = ui_x;              // BG3HOFS
		*(vu16*)0x400001E = ui_y_real;         // BG3VOFS
		dispcnt |= 0x0800;
	}
	*(vu16*)0x4000000 = dispcnt;

	if(g_scale_mode==SCALE_FULL)
	{	// arm HBlank DMA: DMA0 feeds BG0VOFS, DMA1 feeds BG1VOFS.
		// (writing the full CNT re-latches source/dest each frame)
		*(vu32*)0x40000B0 = (u32)&sc_vtab0[1];       // DM0SAD
		*(vu32*)0x40000B4 = 0x04000012;              // DM0DAD = BG0VOFS
		*(vu32*)0x40000B8 = 159 | (0xA240u<<16);     // hblank, repeat, dest-fixed
		if(wenable)
		{
			*(vu32*)0x40000BC = (u32)&sc_vtab1[1];   // DM1SAD
			*(vu32*)0x40000C0 = 0x04000016;          // DM1DAD = BG1VOFS
			*(vu32*)0x40000C4 = 159 | (0xA240u<<16);
		}
		else
			*(vu16*)0x40000C6 = 0;                   // DMA1 off
		*(vu16*)0x40000D2 = 0;                       // DMA2 off
	}
	else
	{	// Stop any HBlank-repeat DMA (normal mode's register writer, or a
		// leftover vertical-scale DMA after a mode switch).
		*(vu16*)0x40000BA = 0;   // DMA0CNT_H
		*(vu16*)0x40000C6 = 0;   // DMA1CNT_H
		*(vu16*)0x40000D2 = 0;   // DMA2CNT_H
	}

	sc_busy=0;
}

// Transform the OAM that display_sprites just wrote into 1.5x-wide affine
// sprites. Called from vblankinterrupt (lcd.s) after display_sprites when
// scaled. Affine sprites have no flip bits (that attr1 field selects the
// affine parameter set), so four sets cover the GB flip combinations.
// PA=170, not 171: 171's edge sampling drops a column (splits multi-sprite
// characters); 170 duplicates the last column and neighbors cover it.
void scaling_fix_oam(void)
{
	vu16 *oam=(vu16*)0x07000000;
	int i;
	int ysh = (lcdctrl0frame_ & 0x04) ? 8 : 4;   // half of 16/8 height
	int vert = (g_scale_mode==SCALE_FULL);
	u16 pd = vert ? 230 : 256;                   // 230 = 256*144/160

	oam[3]  =  170; oam[7]  = 0; oam[11] = 0; oam[15] =  pd;
	oam[19] = (u16)-170; oam[23] = 0; oam[27] = 0; oam[31] =  pd;
	oam[35] =  170; oam[39] = 0; oam[43] = 0; oam[47] = (u16)-pd;
	oam[51] = (u16)-170; oam[55] = 0; oam[59] = 0; oam[63] = (u16)-pd;

	for(i=0;i<40;i++)
	{
		u16 a0=oam[i*4], a1=oam[i*4+1];
		int x, y, flips;
		if((a0&0x0300)==0x0200) continue;        // hidden slot
		x = a1 & 0x1FF;                           // = gbX_hw + 32
		x = ((3*(x-40)+1)>>1) - 2;                // ceil(1.5*(gbX-8)) - box pad
		y = a0 & 0xFF;                            // = srcY + 8 (mod 256)
		if(vert)
		{	// full stretch: no 8px top border, Y scaled by 10/9
			int s = (y<200) ? y-8 : y-8-256;      // signed source Y
			y = (s*10)/9;
		}
		y -= ysh;                                 // center in double-size box
		flips = (a1>>12)&3;                       // GB h/v flip -> affine set
		a1 = (a1 & 0xC000) | (flips<<9) | (x & 0x1FF);
		a0 = (a0 & 0xFC00) | 0x0300 | (y & 0xFF);
		oam[i*4]   = a0;
		oam[i*4+1] = a1;
	}
}
