// scaling.c - tile-domain fractional scaling for jagoombacolor.
// Design + journal: docs/SCALING_TILE_PLAN.md
//
// v14: generic N/D horizontal ratio engine (one output tile = one cache
// cell keyed on source tile pair + phase), replacing the 1.5x-only
// pair->triple scheme. Modes:
//   SCALE_FIT  (1): 180x160  - 9/8 horizontal, 10/9 vertical, 30px borders
//   SCALE_FULL (2): 240x160  - 3/2 horizontal, 10/9 vertical, full screen
// Vertical = per-scanline VOFS via HBlank DMA (line-repeat every ~9th).
// DMG only; CGB attrs/colorization still open.
//
// The visible map is a per-frame VIEWPORT: every frame the ~24-31 visible
// output columns are re-looked-up (cache hits are cheap) and written to
// fixed map slots; HOFS carries only the sub-tile phase. No ring/wrap
// cases. Registers/tables/DMA are set FIRST in the vblank call, heavy
// cache work after, so scanout never starts on half-programmed state.
//
// VRAM while scaled (all rebuilt by scaling_restore on toggle-off):
//   0x06005000-0x57FF  BG0 map 32x32 (screenblock 10)
//   0x06006000-0x67FF  BG1 window map 32x32 (screenblock 12)
//   0x06006800-0xBFFF  cell cache tiles (charbase 1, indices 320..1007)
//   0x06004000-0x4FFF  UNTOUCHED: UI font + UI map
//   0x06014000+        OBJ tiles, kept fresh by our dirty pass

#include "includes.h"
#include "asmcalls.h"
#include "scaling.h"

extern u8 lcdctrl0frame_;
extern u8 gbc_mode;
extern u8 scrollX, scrollY, windowX, windowY;
extern u32 ui_border_request;
extern u32 DIRTY_TILE_BITS[];
extern u8 VRAM_chr_lastAddr;
extern u32 _bigbufferbase2;   // v31: completed frame's per-line records
                              // (6 words/GB line; +2 = xyscroll =
                              // (scy-8)<<16 | (scx-40), what DMA0 would
                              // have replayed in normal mode)

#define SCALED_MAP   ((vu16*)0x06005000)
#define WIN_MAP      ((vu16*)0x06006000)
#define CHARBASE1    ((u8*)0x06004000)
#define OBJ_TILES    ((u8*)0x06014000)
#define TILE_BASE    320
#define MAX_TILES    688            // tiles 320..1007
#define BLANK_TILE   1021
#define BG_PAL       8
#define VIS_ROWS     19
#define HASH_SIZE    2048
#define KEY_TOMB     1

EWRAM_BSS static u32 sc_hkey[HASH_SIZE];
EWRAM_BSS static u16 sc_hval[HASH_SIZE];
EWRAM_BSS static u32 sc_cell_key[MAX_TILES];
EWRAM_BSS static u8  sc_cell_gen[MAX_TILES];
EWRAM_BSS static u32 sc_nib[256];
EWRAM_BSS static u32 sc_nibR[256];  // v33: nibble-reversed expansion for
                                    // h-flipped tiles (same cost path)
EWRAM_BSS static u16 sc_wbuilt[18];
EWRAM_BSS static u32 sc_dirty_snap[12];
EWRAM_BSS static u8  sc_mapdirty[64];   // per-frame snapshot of the map
                                        // dirty bytes: BG and window may
                                        // share a tilemap, so consuming
                                        // dirty_map_words directly lets
                                        // one loop wipe the other's signal
EWRAM_BSS static u8  sc_srcsel[9][8];   // per phase: source nibble 0..15
EWRAM_BSS static u32 sc_ph_m[9][4];     // v30 mask-combine: per phase,
EWRAM_BSS static u8  sc_ph_q4[9];       // masks for off 0..-3 + 4*sel[0]
EWRAM_BSS static u8  sc_vdelta[161];
EWRAM_BSS static u16 sc_vquad[161][4]; // v31: per-line {BG0HOFS,BG0VOFS,
                                       // BG1HOFS,BG1VOFS} DMA0 stream -
                                       // per-band scroll needs per-line
                                       // HOFS, not just VOFS

EWRAM_BSS static u16 sc_row_c0[32]; // per BG row: C0 the row was built for
                                    // (0xFFFF = invalid). Ring viewport:
                                    // slot = column & 63 in the 64x32 map,
                                    // so scrolling reuses all cells and
                                    // only entering columns get built.
static u16 sc_ncells, sc_tombs, sc_evict_ptr;
// (v32: vquad rebuilds every frame from the per-line capture - no tuple)
static u8 sc_gen, sc_tables_ok, sc_active, sc_last_lcdc, sc_last_wy;
static u8 sc_gbc;                   // v33: GBC attribute decoding active
// tile ids 0..767: 0-383 bank0, 384-767 bank1 (matches the dirty-bit
// layout: bit = id/2, bank1 bits start at byte offset 24)
#define SC_TILE_ADDR(t) (XGB_VRAM + (((t)<384)?(t)*16:((t)-384)*16+0x2000))
static u8 sc_N, sc_D;               // current ratio (9/8 or 3/2)
static volatile u8 sc_busy;         // menu-sync vs vblank reentry guard

// Frame work budget: display state must never depend on workload, so cell
// conversions are capped per vblank; unfinished rows/sweeps resume next
// frame (stale content beats torn scanout). Menu toggles build unbounded
// (game paused, overrun invisible).
static int sc_budget;
static u8 sc_menu_build;
static u8 sc_timed;       // v30: budget is time-gated (stop when scanout
                          // passes SC_DEADLINE_LINE), not count-gated
#define SC_DEADLINE_LINE 45
#define SC_TIME_UP() (sc_timed && \
	((*(vu16*)0x4000006) >= SC_DEADLINE_LINE && (*(vu16*)0x4000006) < 160))
static u8 sc_wst_cur;     // debounced window state: 0x80|wtop, 0 = off.
static u8 sc_wst_pend;    // games move WX/WY mid-frame; our once-per-
static u8 sc_wst_cnt;     // vblank sample flip-flops without debounce
static u8 sc_sweep_active;          // dirty-reconvert sweep in progress
static u16 sc_sweep_obj;            // next OBJ tile pair (0..128)
static u16 sc_sweep_cell;           // next cache cell

static void sc_build_tables(void)
{
	int b,i,p,x;
	for(b=0;b<256;b++)
	{
		u32 v=0,w=0;
		for(i=0;i<8;i++)
		{
			u32 bit=(u32)((b>>(7-i))&1);
			v |= bit<<(i*4);
			w |= bit<<((7-i)*4);
		}
		sc_nib[b]=v;
		sc_nibR[b]=w;
	}
	for(b=0;b<161;b++)
		sc_vdelta[b] = b - (9*b)/10;          // single phase (no flicker)
	// nibble-select per phase: output col C == phase (mod N); output px
	// 8C+x shows source px ((8C+x)*D)/N; index relative to the pair start
	for(p=0;p<sc_N;p++)
		for(x=0;x<8;x++)
		{
			int s0=(8*p*sc_D)/sc_N;
			sc_srcsel[p][x] = (u8)(((8*p+x)*sc_D)/sc_N - (s0&~7));
		}
	for(p=0;p<sc_N;p++)
	{	// v30 mask-combine constants: sel[] ascends from q with only
		// repeats (rate 8/9 or 2/3), so output x shows source nibble
		// x+q-off with off in 0..3 -> four fixed masks per phase
		int q=sc_srcsel[p][0];
		sc_ph_q4[p]=(u8)(q*4);
		for(x=0;x<4;x++) sc_ph_m[p][x]=0;
		for(x=0;x<8;x++)
		{
			int off = x + q - sc_srcsel[p][x];
			sc_ph_m[p][off] |= 0xFu<<(x*4);
		}
	}
}

// Convert one output tile: pair (a,b) at phase p -> 8 rows of 8 nibbles
// picked from the 16-nibble source pair rows, pixel values shifted to 1..4.
static void sc_convert_cell(const u8 *ta,const u8 *tb,int p,u32 *dst,
                            int fA,int fB)
{
	// v30 mask-combine: build the 8-nibble source string v starting at
	// nibble q, then place each output from v/v<<4/v<<8/v<<12 by the
	// per-phase masks. ~10 ops/row vs the old per-pixel loop.
	// v33 flips: hflip = the reversed nibble table (same cost), vflip =
	// reversed source row order, per tile (GBC attrs bit5/6).
	u32 q4=sc_ph_q4[p];
	u32 m0=sc_ph_m[p][0],m1=sc_ph_m[p][1];
	u32 m2=sc_ph_m[p][2],m3=sc_ph_m[p][3];
	const u32 *na=(fA&1)?sc_nibR:sc_nib;
	const u32 *nb=(fB&1)?sc_nibR:sc_nib;
	int r;
	for(r=0;r<8;r++)
	{
		int rA=(fA&2)?(7-r):r;
		u32 v = na[ta[rA*2]] | (na[ta[rA*2+1]]<<1);
		if(q4)
		{
			int rB=(fB&2)?(7-r):r;
			v = (v>>q4) | ((nb[tb[rB*2]] | (nb[tb[rB*2+1]]<<1))<<(32-q4));
		}
		dst[r] = ((v&m0) | ((v<<4)&m1) | ((v<<8)&m2) | ((v<<12)&m3))
		         + 0x11111111;
	}
}

static void sc_convert_obj(const u8 *src,u32 *dst)
{
	int r;
	for(r=0;r<8;r++)
		dst[r] = sc_nib[src[r*2]] | (sc_nib[src[r*2+1]]<<1);
}

static u32* sc_cell_vram(u32 c)
{
	return (u32*)(CHARBASE1 + (TILE_BASE + c)*32);
}

static void sc_full_reset(void)
{
	int i;
	u32 *bt=(u32*)(CHARBASE1 + BLANK_TILE*32);
	u16 blank=BLANK_TILE;
	vu16 *m;
	for(i=0;i<HASH_SIZE;i++) sc_hkey[i]=0;
	for(i=0;i<18;i++) sc_wbuilt[i]=0;
	for(i=0;i<32;i++) sc_row_c0[i]=0xFFFF;
	sc_ncells=0; sc_tombs=0; sc_evict_ptr=0;
	for(i=0;i<8;i++) bt[i]=0;
	m=SCALED_MAP; for(i=0;i<0x800;i++) m[i]=blank;   // both blocks (64x32)
	m=WIN_MAP;    for(i=0;i<0x400;i++) m[i]=blank;
}

static void sc_rehash(void)
{
	int i;
	for(i=0;i<HASH_SIZE;i++) sc_hkey[i]=0;
	sc_tombs=0;
	for(i=0;i<sc_ncells;i++)
	{
		u32 key=sc_cell_key[i];
		u32 h=(key*0x9E3779B1u >> 20)&(HASH_SIZE-1);
		while(sc_hkey[h]) h=(h+1)&(HASH_SIZE-1);
		sc_hkey[h]=key; sc_hval[h]=i;
	}
}

static void sc_hash_del(u32 key)
{
	u32 h=(key*0x9E3779B1u >> 20)&(HASH_SIZE-1);
	while(sc_hkey[h])
	{
		if(sc_hkey[h]==key) { sc_hkey[h]=KEY_TOMB; sc_tombs++; return; }
		h=(h+1)&(HASH_SIZE-1);
	}
}

static u32 sc_alloc_cell(void)
{
	int n,pass;
	if(sc_ncells<MAX_TILES) return sc_ncells++;
	// v32 two-pass: gentle first (age>16), then age>8. The stamp rotation
	// re-marks every DISPLAYED cell within 8 frames, so age>8 is safe to
	// take. v31's single >16 gate made a full cache wait out the age
	// quantum for every entering scroll column = the 4Hz stall trains
	// (15-frame period == the eviction age, confirmed by capture FFT).
	for(pass=0;pass<2;pass++)
	{
		u8 lim = pass ? 8 : 16;
		for(n=0;n<MAX_TILES;n++)
		{
			u32 c=sc_evict_ptr;
			sc_evict_ptr = (sc_evict_ptr+1)==MAX_TILES ? 0 : sc_evict_ptr+1;
			if((u8)(sc_gen - sc_cell_gen[c]) > lim)
			{
				sc_hash_del(sc_cell_key[c]);
				return c;
			}
		}
	}
	return MAX_TILES;
}

// key: bit28 marker | a(10b)<<18 | b(10b)<<8 | flips(4b)<<4 | phase(4b)
// (v33: tile ids carry the VRAM bank, flips carry GBC attr bits 5-6)
static u32 sc_lookup(u32 a,u32 b,u32 p,u32 f)
{
	u32 key = (1u<<28)|(a<<18)|(b<<8)|(f<<4)|p;
	u32 h = (key*0x9E3779B1u >> 20)&(HASH_SIZE-1);
	u32 first=HASH_SIZE, c;
	for(;;)
	{
		u32 k=sc_hkey[h];
		if(k==key)
		{
			c=sc_hval[h];
			sc_cell_gen[c]=sc_gen;
			return TILE_BASE+c;
		}
		if(k==KEY_TOMB) { if(first==HASH_SIZE) first=h; }
		else if(k==0)   { if(first==HASH_SIZE) first=h; break; }
		h=(h+1)&(HASH_SIZE-1);
	}
	if(sc_budget<=0) return 0;      // out of frame budget: caller retries
	if(SC_TIME_UP()) { sc_budget=0; return 0; }
	c=sc_alloc_cell();
	if(c>=MAX_TILES) return 0;      // cache full: RETRY too - hidden rows
	                                // age past the eviction threshold in
	                                // <=16 frames and space appears. (A
	                                // permanent BLANK here left menus
	                                // stably glitched: rows completed
	                                // with blanks and never rebuilt.)
	sc_budget--;
	sc_convert_cell(SC_TILE_ADDR(a), SC_TILE_ADDR(b), p, sc_cell_vram(c),
	                f&3, f>>2);
	sc_cell_key[c]=key; sc_cell_gen[c]=sc_gen;
	if(sc_hkey[first]==KEY_TOMB) sc_tombs--;
	sc_hkey[first]=key; sc_hval[first]=c;
	if(sc_tombs>HASH_SIZE/2) sc_rehash();
	return TILE_BASE+c;
}

void scaling_enter(void)
{
	sc_active=0;
	sc_tables_ok=0;   // ratio may change between modes: rebuild tables
	sc_menu_build=1;  // the synchronous menu build runs unbudgeted
}

#define TILE_DIRTY(t) ((sc_dirty_snap[(t)>>6] >> (((t)>>1)&31)) & 1)

static void sc_update_dirty(void)
{
	int i;
	u32 acc=0;
	for(i=0;i<12;i++)
	{	// ACCUMULATE into the snapshot: it persists until a sweep finishes
		u32 d=DIRTY_TILE_BITS[i];
		if(d) { sc_dirty_snap[i]|=d; acc|=d; DIRTY_TILE_BITS[i]=0; }
	}
	if(acc)
	{	// v26: RESTART the sweep on any merge - a one-shot write landing
		// behind an active sweep's cursor was silently lost until eviction
		VRAM_chr_lastAddr=0xFF;
		sc_sweep_active=1;
		sc_sweep_obj=0;
		sc_sweep_cell=0;
#if SCALING_DEBUG
		*(vu16*)0x05000000 = 0x7C1F;   // CAUSE DEBUG: magenta = sweep restart
#endif
	}
	if(!sc_sweep_active) return;

	// OBJ tiles first, on their OWN budget: OAM may already reference the
	// new tile index this frame, so a deferred OBJ convert = torn sprite.
	{
		int obudget=64;               // v30: conversions are cheap now
		while(sc_sweep_obj<128 && obudget>0 && !SC_TIME_UP())
		{
			u32 t=sc_sweep_obj*2;
			sc_sweep_obj++;
			if(!TILE_DIRTY(t)) continue;
			sc_convert_obj(XGB_VRAM+t*16,           (u32*)(OBJ_TILES+t*32));
			sc_convert_obj(XGB_VRAM+(t+1)*16,       (u32*)(OBJ_TILES+(t+1)*32));
			sc_convert_obj(XGB_VRAM+0x2000+t*16,    (u32*)(OBJ_TILES+0x2000+t*32));
			sc_convert_obj(XGB_VRAM+0x2000+(t+1)*16,(u32*)(OBJ_TILES+0x2000+(t+1)*32));
			obudget--;
		}
	}
	while(sc_sweep_cell<sc_ncells && sc_budget>0)
	{
		u32 key=sc_cell_key[sc_sweep_cell], a, b;
		a=(key>>18)&0x3FF; b=(key>>8)&0x3FF;
		if(key && (TILE_DIRTY(a) || TILE_DIRTY(b)))
		{
			if((u8)(sc_gen - sc_cell_gen[sc_sweep_cell]) > 8)
			{	// v30 visible-first: a dirty cell nobody stamped within
				// the 8-frame rotation isn't on screen - evict it instead
				// of paying a reconvert (tile-streaming games invalidate
				// far more cache than they display)
				sc_hash_del(key);
				sc_cell_key[sc_sweep_cell]=0;
				sc_cell_gen[sc_sweep_cell]=(u8)(sc_gen-64);
				// v31: evictions are cheap but not free (hash probes,
				// tombstone pressure) - gate them like conversions
				sc_budget--;
				if(SC_TIME_UP()) sc_budget=0;
			}
			else
			{
				sc_convert_cell(SC_TILE_ADDR(a), SC_TILE_ADDR(b),
					(int)(key&0xF), sc_cell_vram(sc_sweep_cell),
					(int)((key>>4)&3), (int)((key>>6)&3));
				sc_budget--;
				if(SC_TIME_UP()) sc_budget=0;
			}
		}
		sc_sweep_cell++;
	}
	if(sc_sweep_obj>=128 && sc_sweep_cell>=sc_ncells)
	{	// sweep complete: everything converted from current tile data
		sc_sweep_active=0;
		for(i=0;i<12;i++) sc_dirty_snap[i]=0;
	}
}

// Look up the output tile for content column C (wrapped) of source row mrow.
static u16 sc_cell_entry(const u8 *mrow,int C,int mode8000,int ccols)
{
	u32 a,b,p,s0,aa,ab,f;
	while(C>=ccols) C-=ccols;
	s0=(8*C*sc_D)/sc_N;
	{
		int ia=(s0>>3)&31, ib=((s0>>3)+1)&31;
		a=mrow[ia];
		b=mrow[ib];
		// v33: GBC attribute map is the same offsets in VRAM bank 1
		// (+0x2000). Mixed-palette pairs take tile A's palette for the
		// whole cell - straddle slivers only, attrs cluster by object.
		aa = sc_gbc ? mrow[ia+0x2000] : 0;
		ab = sc_gbc ? mrow[ib+0x2000] : 0;
	}
	p=C%sc_N;
	if(!mode8000)
	{
		if(a<128) a+=256;
		if(b<128) b+=256;
	}
	if(aa&8) a+=384;               // VRAM bank attr
	if(ab&8) b+=384;
	f=((aa>>5)&3)|(((ab>>5)&3)<<2);
	{
		u32 t=sc_lookup(a,b,p,f);
		if(!t) return 0;            // budget exhausted: retry next frame
		return (u16)(t | ((aa&7)<<12));
	}
}

// Write one entry into the 64x32 ring map: slot 32..63 = second block.
static void sc_map_put(int mr,int slot,u16 e)
{
	slot&=63;
	SCALED_MAP[mr*32 + (slot&31) + ((slot&32)<<5)] = e;
}
static u16 sc_map_get(int mr,int slot)
{
	slot&=63;
	return SCALED_MAP[mr*32 + (slot&31) + ((slot&32)<<5)];
}
static void sc_stamp_slot(int mr,int slot)
{
	u32 t=sc_map_get(mr,slot)&0x3FF;
	if(t>=TILE_BASE && t<TILE_BASE+MAX_TILES)
		sc_cell_gen[t-TILE_BASE]=sc_gen;
}

// v33: scaled BG uses GBA palette rows 0-7 = the 8 GBC BG palettes at
// entries 1-4 (cell pixels 1..4 = GB colors 0..3; map entry carries the
// row from the tile's attr). Normal mode keeps rows 8-15 (gamma copies
// from transfer_palette_) - no conflict. DMG games use row 0 only.
static void sc_load_palette(void)
{
	vu16 *pal=(vu16*)0x05000000;
	u16 *src=(u16*)gbc_palette;
	int p,n = gbc_mode ? 8 : 1;
	for(p=0;p<n;p++)
	{
		pal[p*16+1]=src[p*4+0]; pal[p*16+2]=src[p*4+1];
		pal[p*16+3]=src[p*4+2]; pal[p*16+4]=src[p*4+3];
	}
	pal[0]=0;
}

void scaling_scaled_frame(void)
{
	u8 lcdc;
	const u8 *gbmap, *wmap;
	u8 *wdirty, *mdirty;
	int mode8000,r,j;
	int wenable,wtop,fit;
	int ccols,ncontent,guardL,guardR;
	u32 *sb;
	u16 dispcnt, blank;

	if(sc_busy)
	{	// mid-build reentry: skip work but keep the display coherent
		// (DMA0 must be disabled+re-armed: source only re-latches on a
		// 0->1 enable transition)
		*(vu16*)0x40000BA = 0;
		*(vu32*)0x40000B0 = (u32)&sc_vquad[1][0];
		*(vu32*)0x40000B4 = 0x04000010;
		*(vu32*)0x40000B8 = 4 | (0xA260u<<16);
		*(vu16*)0x4000010 = sc_vquad[0][0];
		*(vu16*)0x4000012 = sc_vquad[0][1];
		*(vu16*)0x4000014 = sc_vquad[0][2];
		*(vu16*)0x4000016 = sc_vquad[0][3];
		// v27: keep OUR palette in force - transfer_palette_ ran just
		// before us with gamma values; skipping this made busy frames
		// flash gamma-gray
		sc_load_palette();
		return;
	}
	sc_busy=1;
#if SCALING_DEBUG
	*(vu16*)0x05000000 = 0x001F;   // TIMING DEBUG: red while building
#else
	*(vu16*)0x05000000 = 0;        // pin the backdrop black NOW:
	                               // transfer_palette_ ran just before us
	                               // (gamma/white-palette values) and the
	                               // borders blink on long frames if the
	                               // black write only happens at build end
#endif

	// ---- PHASE A: display programming, guaranteed early ----
	fit = (g_scale_mode==SCALE_FIT);
	sc_N = fit ? 9 : 3;
	sc_D = fit ? 8 : 2;
	if(!sc_tables_ok) { sc_build_tables(); sc_tables_ok=1; }

	lcdc = lcdctrl0frame_;
	sc_gbc = gbc_mode;
	ccols = (32*sc_N)/sc_D;                    // 36 (fit) / 48 (stretch)
	ncontent = fit ? 24 : 31;
	guardL   = fit ? 4  : 0;
	guardR   = fit ? 5  : 1;
	// v32: full per-line scroll from the capture the GB core fills for
	// the normal renderer's scanline DMA (xyscroll word per GB line:
	// (scy-8)<<16 | (scx-40), 6-word stride). v31's two-band model
	// couldn't express Batman's title spin = a different SCY on nearly
	// every scanline of a static map.
	sb=(u32*)_bigbufferbase2;
	{	// window state with 3-frame debounce (mid-frame WX/WY tricks make
		// the raw per-vblank sample oscillate)
		int rawen = (lcdc&0x20) && windowY<144 && windowX<8;
		u8 raw = rawen ? (u8)(0x80 | (windowY>>3)) : 0;
		if(raw==sc_wst_cur)
			sc_wst_cnt=0;
		else if(raw==sc_wst_pend)
		{
			if(++sc_wst_cnt>=3) { sc_wst_cur=raw; sc_wst_cnt=0; }
		}
		else { sc_wst_pend=raw; sc_wst_cnt=1; }
		wenable = sc_wst_cur>>7;
		wtop = sc_wst_cur & 0x1F;
	}

	{	// vquad fill: per-line {HOFS0,VOFS0,HOFS1,VOFS1} straight from the
		// captured table. Output line y shows source GB line g=y-delta[y];
		// the game wanted map line scyL[g]+g there, so VOFS(y) =
		// scyL[g]-delta[y]. Rebuilt every frame (160 iterations, cheap).
		// VOFS1 keeps the v29 wsub sub-tile window seat.
		int y, xb=fit?30:0;
		int wsub = wenable ? (windowY&7) : 0;
		u16 h1=(u16)((0-xb)&0x1FF);
		for(y=0;y<161;y++)
		{
			int g=y-sc_vdelta[y];
			u32 v;
			if(g>143) g=143;
			v=sb[g*6+2];
			sc_vquad[y][0]=(u16)(((((v+40)&0xFF)*sc_N)/sc_D - xb)&0x1FF);
			sc_vquad[y][1]=(u16)((((v>>16)+8) - sc_vdelta[y])&0xFF);
			sc_vquad[y][2]=h1;
			sc_vquad[y][3]=(u16)((0-sc_vdelta[y]-wsub)&0xFF);
		}
	}

	REG_BG0CNT = 3 | (1<<2) | (10<<8) | (1<<14);            // 64x32 ring
	*(vu16*)0x4000010 = sc_vquad[0][0];
	*(vu16*)0x4000012 = sc_vquad[0][1];

	dispcnt = 0x1140;
	if(wenable)
	{
		*(vu16*)0x400000A = 2 | (1<<2) | (12<<8);
		*(vu16*)0x4000014 = sc_vquad[0][2];
		*(vu16*)0x4000016 = sc_vquad[0][3];
		dispcnt |= 0x0200;
	}
	if(ui_border_request & 1)
	{
		*(vu16*)0x400000E = (9<<8) | (1<<2);
		*(vu16*)0x400001C = ui_x;
		*(vu16*)0x400001E = ui_y_real;
		dispcnt |= 0x0800;
	}
	if(fit)
	{	// v28: clip to the 180px pane. Cells are fully opaque (pixels 1..4)
		// and the 24-cell viewport is wider than 180px, so without a window
		// live content bleeds into the letterbox borders
		*(vu16*)0x4000042 = (30<<8) | 210;     // WIN1H
		*(vu16*)0x4000046 = 160;               // WIN1V 0..160
		*(vu16*)0x4000048 = 0x1B00;            // WININ: win1 = BG0|BG1|BG3|OBJ
		*(vu16*)0x400004A = 0x0008;            // WINOUT: border BG3 only
		dispcnt |= 0x4000;
	}
	*(vu16*)0x4000000 = dispcnt;

	*(vu16*)0x40000BA = 0;                     // disable-first: relatch SAD
	*(vu32*)0x40000B0 = (u32)&sc_vquad[1][0];
	*(vu32*)0x40000B4 = 0x04000010;
	// count MUST equal regs-per-line (v28 lesson: an HBlank-repeat DMA
	// transfers its whole count every hblank, count reloads, source keeps
	// advancing). v31: 4 halfwords {HOFS0,VOFS0,HOFS1,VOFS1}, dest
	// inc+reload back to 0x4000010 each line.
	*(vu32*)0x40000B8 = 4 | (0xA260u<<16);
	*(vu16*)0x40000C6 = 0;                     // DMA1 retired in v31
	*(vu16*)0x40000D2 = 0;

	sc_load_palette();

	// ---- PHASE B: budgeted build work (may run past vblank safely) ----
	// v30: the real limit is time (SC_TIME_UP in the convert paths); the
	// count is a runaway backstop. Old fixed 64 starved tile-streaming
	// games into multi-frame stale cells.
	sc_budget = sc_menu_build ? 0x7FFFFFFF : 4096;
	sc_timed  = !sc_menu_build;
	sc_menu_build = 0;
	if(!sc_active)
	{
		int i;
		sc_full_reset();
		sc_active=1;
		for(i=0;i<12;i++) DIRTY_TILE_BITS[i]=0;
		VRAM_chr_lastAddr=0xFF;
	}
	sc_gen++;
	sc_update_dirty();

	gbmap  = XGB_VRAM + ((lcdc&0x08) ? 0x1C00 : 0x1800);
	mode8000 = lcdc&0x10;
	{	// snapshot + clear the map dirty bytes once per frame; both the
		// BG and window loops read the snapshot (may be the same map!)
		int i;
		for(i=0;i<64;i++)
		{
			u8 d=dirty_map_words[i];
			sc_mapdirty[i]=d;
			if(d) dirty_map_words[i]=0;
		}
	}
	mdirty = sc_mapdirty + ((lcdc&0x08) ? 32 : 0);
	wmap   = XGB_VRAM + ((lcdc&0x40) ? 0x1C00 : 0x1800);
	wdirty = sc_mapdirty + ((lcdc&0x40) ? 32 : 0);
	blank = BLANK_TILE;
	if(((lcdc ^ sc_last_lcdc) & 0x58) || sc_wst_cur != sc_last_wy)
	{	// v26: NO full wipe on window change - the old entries stay
		// displayed until each row's replacement is built (same rule as
		// BG rows). Only rows ABOVE the new window top get blanked (they
		// left the window and must be transparent).
		int i, lim = wenable ? wtop : 18;
		vu16 *m=WIN_MAP;
#if SCALING_DEBUG
		*(vu16*)0x05000000 = 0x7C00;   // CAUSE DEBUG: blue = lcdc/window
		                               // change -> full row invalidation
#endif
		for(i=0;i<lim*32;i++) m[i]=blank;
		for(i=0;i<18;i++) sc_wbuilt[i]=0;
		// BG rows hidden behind the window aren't stamped and may have
		// been evicted: force rebuild when the window layout changes
		for(i=0;i<32;i++) sc_row_c0[i]=0xFFFF;
	}
	sc_last_lcdc=lcdc; sc_last_wy=sc_wst_cur;   // (sc_last_wy now tracks
	                                            // the debounced state)

	{	// blank-tile insurance
		u32 *bt=(u32*)(CHARBASE1 + BLANK_TILE*32);
		int i;
		for(i=0;i<8;i++) bt[i]=0;
	}

	// ---- BG0 ring viewport (v32: build exactly the map rows the
	// per-line table displays this frame; per-line effects like the
	// Batman title spin touch many rows, all resolved by cache hits
	// after the first frame since the map content is static) ----
	{
		u32 rowmask=0;
		u8 rowscx[32], rowy[32];
		int y;
		for(y=0;y<160;y++)
		{
			int g=y-sc_vdelta[y];
			u32 v=sb[g*6+2];
			int mr=(((int)(((v>>16)+8)&0xFF)+g)>>3)&31;
			if(!((rowmask>>mr)&1))
			{
				rowmask|=1u<<mr;
				rowscx[mr]=(u8)((v+40)&0xFF);
				rowy[mr]=(u8)y;
			}
		}
		for(r=0;r<32;r++)
		{
			int mr=r;
			int P_,C0_;
			const u8 *mrow;
			u16 st;
			if(!((rowmask>>mr)&1)) continue;
			if(wenable)
			{	// rows first shown below the window top: hidden, skip
				int ywin=((wtop*8)*10)/9;
				if(rowy[mr] > ywin+9) continue;
			}
			P_=(rowscx[mr]*sc_N)/sc_D;
			C0_=P_>>3;
			st = sc_row_c0[mr];
			if(mdirty[mr]) { st=0xFFFF; sc_row_c0[mr]=0xFFFF; }
			// (persist the invalidation NOW: if the rebuild below runs out
			// of budget, the row must stay invalid for next frame - v26)
			if(st==(u16)C0_)
			{	// valid: rotate eviction stamps (1/8 of rows per frame)
				if(((mr^sc_gen)&7)==0)
					for(j=0;j<ncontent;j++) sc_stamp_slot(mr,C0_+j);
				continue;
			}
			// rebuilding: the row's OLD cells are still on screen this
			// frame - stamp them every frame or a slow convergence gets its
			// displayed cells evicted underneath it (menu-churn loop)
			for(j=0;j<ncontent;j++) sc_stamp_slot(mr,C0_+j);
			mrow = gbmap + mr*32;
			if(st!=0xFFFF && (st==(u16)(C0_-1) || st==(u16)(C0_+1)))
			// (st==0xFFFF must never classify as a pan: at C0==0 the
			// invalid sentinel equals (u16)(C0-1) - v26 fix)
			{	// panning: build only the entering column
				int A = (st==(u16)(C0_-1)) ? C0_+ncontent-1 : C0_;
				u16 e = sc_cell_entry(mrow,A,mode8000,ccols);
				if(!e) continue;               // budget: retry next frame
				sc_map_put(mr,A,e);
			}
			else
			{	// teleport/new row: full rebuild (budget-gated)
				int complete=1;
				for(j=0;j<ncontent;j++)
				{
					u16 e = sc_cell_entry(mrow,C0_+j,mode8000,ccols);
					if(!e) { complete=0; continue; }
					sc_map_put(mr,C0_+j,e);
				}
				if(!complete) continue;
			}
			for(j=1;j<=guardL;j++) sc_map_put(mr,C0_-j,blank);
			for(j=0;j<guardR;j++)  sc_map_put(mr,C0_+ncontent+j,blank);
			sc_row_c0[mr]=(u16)C0_;
		}
	}

	// ---- BG1 window layer (screen-fixed) ----
	if(wenable)
	{
		int wcols = fit ? 23 : 30;
		for(r=wtop;r<18;r++)
		{
			int wr=r-wtop;
			const u8 *mrow = wmap + wr*32;
			vu16 *out = WIN_MAP + r*32;
			if(wdirty[wr]) sc_wbuilt[r]=0;
			if(sc_wbuilt[r])
			{
				if(((r^sc_gen)&7)==0)
					for(j=0;j<wcols;j++)
					{
						u32 t=out[j]&0x3FF;
						if(t>=TILE_BASE && t<TILE_BASE+MAX_TILES)
							sc_cell_gen[t-TILE_BASE]=sc_gen;
					}
			}
			else
			{
				int complete=1;
				// displayed old cells must survive the convergence
				for(j=0;j<wcols;j++)
				{
					u32 t=out[j]&0x3FF;
					if(t>=TILE_BASE && t<TILE_BASE+MAX_TILES)
						sc_cell_gen[t-TILE_BASE]=sc_gen;
				}
				for(j=0;j<wcols;j++)
				{
					u16 e = sc_cell_entry(mrow,j,mode8000,ccols);
					if(!e) { complete=0; continue; }
					out[j] = e;
				}
				if(complete) sc_wbuilt[r]=1;
			}
		}
	}

	*(vu16*)0x05000000 = 0;        // TIMING DEBUG: work done -> black
	sc_busy=0;
}

// OAM post-pass: affine scale sprites to the current mode's ratios.
// Four affine sets cover GB flip combos (affine sprites have no flip bits).
// PA rounds DOWN (duplicate edge column, neighbors overlap it) - same for PD.
void scaling_fix_oam(void)
{
	vu16 *oam=(vu16*)0x07000000;
	int i;
	int ysh = (lcdctrl0frame_ & 0x04) ? 8 : 4;
	int fit = (g_scale_mode==SCALE_FIT);
#if SCALING_DEBUG
	*(vu16*)0x05000000 = 0x03E0;   // TIMING DEBUG: green during OAM pass
#endif
	u16 pa = fit ? 227 : 170;                  // 256*8/9 / 256*2/3
	u16 pd = 227;                              // v29: was 230 (256*9/10).
	// Stacked 8px OBJ units sit 9px apart after 10/9 rounding, but PD=230
	// painted only 8.90px of content per unit -> 1px background seam
	// severing tall sprites at some Y phases. 227 paints 9.02px (same
	// round-down-to-overlap rule as PA); the ~1.3% extra height is
	// invisible, the seam is not.
	int xb = fit ? 30 : 0;                     // left border offset

	oam[3]=pa;        oam[7]=0;  oam[11]=0; oam[15]=pd;
	oam[19]=(u16)-pa; oam[23]=0; oam[27]=0; oam[31]=pd;
	oam[35]=pa;       oam[39]=0; oam[43]=0; oam[47]=(u16)-pd;
	oam[51]=(u16)-pa; oam[55]=0; oam[59]=0; oam[63]=(u16)-pd;

	for(i=0;i<40;i++)
	{
		u16 a0=oam[i*4], a1=oam[i*4+1];
		int x,y,flips,s;
		if((a0&0x0300)==0x0200) continue;
		x = a1 & 0x1FF;                        // = gbX_hw + 32
		s = x-40;                              // source screen x
		x = fit ? (xb + ((s*9+7)>>3) - 3)      // ceil(9s/8) - pad(16-9)/2
		        : (((3*s+1)>>1) - 2);          // ceil(3s/2) - pad(16-12)/2
		y = a0 & 0xFF;
		s = (y<200) ? y-8 : y-8-256;           // signed source y
		y = (s*10)/9 - ysh;
		flips = (a1>>12)&3;
		a1 = (a1 & 0xC000) | (flips<<9) | (x & 0x1FF);
		a0 = (a0 & 0xFC00) | 0x0300 | (y & 0xFF);
		oam[i*4]   = a0;
		oam[i*4+1] = a1;
		{	// v26: normal sprites (GBA prio 2 from display_sprites) must
			// beat the window layer (BG1 prio 2; BG wins ties on GBA) -
			// remap to prio 1. "Behind BG" sprites (prio 3) stay.
			u16 a2=oam[i*4+2];
			if((a2&0x0C00)==0x0800)
				oam[i*4+2]=(u16)((a2&~0x0C00)|0x0400);
		}
	}
	*(vu16*)0x05000000 = 0;        // TIMING DEBUG: all vblank work done
}
