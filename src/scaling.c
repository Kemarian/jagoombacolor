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
extern u8 scrollX, scrollY, windowX, windowY;
extern u32 ui_border_request;
extern u32 DIRTY_TILE_BITS[];
extern u8 VRAM_chr_lastAddr;

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
EWRAM_BSS static u16 sc_wbuilt[18];
EWRAM_BSS static u32 sc_dirty_snap[12];
EWRAM_BSS static u8  sc_mapdirty[64];   // per-frame snapshot of the map
                                        // dirty bytes: BG and window may
                                        // share a tilemap, so consuming
                                        // dirty_map_words directly lets
                                        // one loop wipe the other's signal
EWRAM_BSS static u8  sc_srcsel[9][8];   // per phase: source nibble 0..15
EWRAM_BSS static u8  sc_vdelta[161];
EWRAM_BSS static u16 sc_vtab0[161];
EWRAM_BSS static u16 sc_vtab1[161];

EWRAM_BSS static u16 sc_row_c0[32]; // per BG row: C0 the row was built for
                                    // (0xFFFF = invalid). Ring viewport:
                                    // slot = column & 63 in the 64x32 map,
                                    // so scrolling reuses all cells and
                                    // only entering columns get built.
static u16 sc_ncells, sc_tombs, sc_evict_ptr;
static int sc_last_scy=-1;
static u8 sc_wsub;                  // v29: window sub-tile Y (WY&7) baked
                                    // into sc_vtab1; rebuild on change
static u8 sc_gen, sc_tables_ok, sc_active, sc_last_lcdc, sc_last_wy;
static u8 sc_N, sc_D;               // current ratio (9/8 or 3/2)
static volatile u8 sc_busy;         // menu-sync vs vblank reentry guard

// Frame work budget: display state must never depend on workload, so cell
// conversions are capped per vblank; unfinished rows/sweeps resume next
// frame (stale content beats torn scanout). Menu toggles build unbounded
// (game paused, overrun invisible).
static int sc_budget;
static u8 sc_menu_build;
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
		u32 v=0;
		for(i=0;i<8;i++)
			v |= ((u32)((b>>(7-i))&1))<<(i*4);
		sc_nib[b]=v;
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
}

// Convert one output tile: pair (a,b) at phase p -> 8 rows of 8 nibbles
// picked from the 16-nibble source pair rows, pixel values shifted to 1..4.
static void sc_convert_cell(const u8 *ta,const u8 *tb,int p,u32 *dst)
{
	const u8 *sel=sc_srcsel[p];
	int r,x;
	for(r=0;r<8;r++)
	{
		u32 ra = sc_nib[ta[r*2]] | (sc_nib[ta[r*2+1]]<<1);
		u32 rb = sc_nib[tb[r*2]] | (sc_nib[tb[r*2+1]]<<1);
		u32 o=0;
		for(x=0;x<8;x++)
		{
			int s=sel[x];
			u32 n = (s<8) ? (ra>>(s*4)) : (rb>>((s-8)*4));
			o |= (n&0xF)<<(x*4);
		}
		dst[r] = o + 0x11111111;
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
	u16 blank=BLANK_TILE|(BG_PAL<<12);
	vu16 *m;
	for(i=0;i<HASH_SIZE;i++) sc_hkey[i]=0;
	for(i=0;i<18;i++) sc_wbuilt[i]=0;
	for(i=0;i<32;i++) sc_row_c0[i]=0xFFFF;
	sc_ncells=0; sc_tombs=0; sc_evict_ptr=0;
	sc_last_scy=-1;
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
	int n;
	if(sc_ncells<MAX_TILES) return sc_ncells++;
	for(n=0;n<MAX_TILES;n++)
	{
		u32 c=sc_evict_ptr;
		sc_evict_ptr = (sc_evict_ptr+1)==MAX_TILES ? 0 : sc_evict_ptr+1;
		// stamps rotate over 8 frames now: only evict clearly old cells
		if((u8)(sc_gen - sc_cell_gen[c]) > 16)
		{
			sc_hash_del(sc_cell_key[c]);
			return c;
		}
	}
	return MAX_TILES;
}

// key: bit24 marker | a(9b)<<15 | b(9b)<<6 | phase(4b)
static u32 sc_lookup(u32 a,u32 b,u32 p)
{
	u32 key = (1u<<24)|(a<<15)|(b<<6)|p;
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
	c=sc_alloc_cell();
	if(c>=MAX_TILES) return 0;      // cache full: RETRY too - hidden rows
	                                // age past the eviction threshold in
	                                // <=16 frames and space appears. (A
	                                // permanent BLANK here left menus
	                                // stably glitched: rows completed
	                                // with blanks and never rebuilt.)
	sc_budget--;
	sc_convert_cell(XGB_VRAM+a*16, XGB_VRAM+b*16, p, sc_cell_vram(c));
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
	}
	if(!sc_sweep_active) return;

	// OBJ tiles first, on their OWN budget: OAM may already reference the
	// new tile index this frame, so a deferred OBJ convert = torn sprite.
	{
		int obudget=24;
		while(sc_sweep_obj<128 && obudget>0)
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
		a=(key>>15)&0x1FF; b=(key>>6)&0x1FF;
		if(TILE_DIRTY(a) || TILE_DIRTY(b))
		{
			sc_convert_cell(XGB_VRAM+a*16, XGB_VRAM+b*16, key&0xF,
				sc_cell_vram(sc_sweep_cell));
			sc_budget--;
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
	u32 a,b,p,s0;
	while(C>=ccols) C-=ccols;
	s0=(8*C*sc_D)/sc_N;
	a=mrow[(s0>>3)&31];
	b=mrow[((s0>>3)+1)&31];
	p=C%sc_N;
	if(!mode8000)
	{
		if(a<128) a+=256;
		if(b<128) b+=256;
	}
	{
		u32 t=sc_lookup(a,b,p);
		if(!t) return 0;            // budget exhausted: retry next frame
		return (u16)(t | (BG_PAL<<12));
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

void scaling_scaled_frame(void)
{
	u8 lcdc;
	const u8 *gbmap, *wmap;
	u8 *wdirty, *mdirty;
	int mode8000,r,j,scy,scx,row0;
	int wenable,wtop,fit;
	int P,C0,phase,ccols,ncontent,guardL,guardR;
	u16 dispcnt, blank;

	if(sc_busy)
	{	// mid-build reentry: skip work but keep the display coherent
		// (both VOFS DMAs must be disabled+re-armed: source only
		// re-latches on a 0->1 enable transition)
		*(vu16*)0x40000BA = 0;
		*(vu32*)0x40000B0 = (u32)&sc_vtab0[1];
		*(vu32*)0x40000B4 = 0x04000012;
		*(vu32*)0x40000B8 = 1 | (0xA240u<<16);
		*(vu16*)0x4000012 = sc_vtab0[0];
		*(vu16*)0x40000C6 = 0;
		if(sc_wst_cur>>7)   // debounced window state
		{
			*(vu32*)0x40000BC = (u32)&sc_vtab1[1];
			*(vu32*)0x40000C0 = 0x04000016;
			*(vu32*)0x40000C4 = 1 | (0xA240u<<16);
			*(vu16*)0x4000016 = sc_vtab1[0];
		}
		{	// v27: keep OUR palette in force - transfer_palette_ ran just
			// before us with gamma values; skipping this write made every
			// busy frame flash gamma-gray
			vu16 *pal=(vu16*)0x05000100;
			u16 *src=(u16*)gbc_palette;
			pal[1]=src[0]; pal[2]=src[1]; pal[3]=src[2]; pal[4]=src[3];
			*(vu16*)0x05000000 = 0;
		}
		return;
	}
	sc_busy=1;

	// ---- PHASE A: display programming, guaranteed early ----
	fit = (g_scale_mode==SCALE_FIT);
	sc_N = fit ? 9 : 3;
	sc_D = fit ? 8 : 2;
	if(!sc_tables_ok) { sc_build_tables(); sc_tables_ok=1; }

	lcdc = lcdctrl0frame_;
	scy = scrollY; scx = scrollX;
	ccols = (32*sc_N)/sc_D;                    // 36 (fit) / 48 (stretch)
	ncontent = fit ? 24 : 31;
	guardL   = fit ? 4  : 0;
	guardR   = fit ? 5  : 1;
	P  = (scx*sc_N)/sc_D;
	C0 = P>>3;
	phase = P&7;
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

	{	// v29: sub-tile window seating. The window map row is tile-
		// quantized (wtop=WY>>3), which parked the ROUND bar 7 rows high
		// (WY=135) with its tilemap row 1 (white panel body) visible below
		// = the "white bar". The per-line VOFS stream absorbs the
		// remainder: shifting vtab1 by WY&7 seats the window exactly.
		// (wsub is undebounced - during slides it may lead wtop by <=7px
		// for the 3 debounce frames; exact at rest, which is what shows.)
		int wsub = wenable ? (windowY&7) : 0;
		if(scy != sc_last_scy || wsub != sc_wsub)
		{
			int y;
			for(y=0;y<161;y++)
			{
				sc_vtab0[y] = (u16)((scy - sc_vdelta[y]) & 0xFF);
				sc_vtab1[y] = (u16)((0   - sc_vdelta[y] - wsub) & 0xFF);
			}
			sc_last_scy = scy;
			sc_wsub = (u8)wsub;
		}
	}

	REG_BG0CNT = 3 | (1<<2) | (10<<8) | (1<<14);            // 64x32 ring
	*(vu16*)0x4000010 = (u16)(((C0&63)*8 + phase - (fit?30:0)) & 0x1FF);
	*(vu16*)0x4000012 = sc_vtab0[0];

	dispcnt = 0x1140;
	if(wenable)
	{
		*(vu16*)0x400000A = 2 | (1<<2) | (12<<8);
		*(vu16*)0x4000014 = (u16)(fit ? -30 : 0);
		*(vu16*)0x4000016 = sc_vtab1[0];
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
	*(vu32*)0x40000B0 = (u32)&sc_vtab0[1];
	*(vu32*)0x40000B4 = 0x04000012;
	// count MUST be 1: an HBlank-repeat DMA transfers its whole count every
	// hblank (count reloads, source keeps advancing). 159 here sprayed the
	// source through 51KB of EWRAM per frame - VOFS became whatever byte the
	// walk ended on (v27: no vertical stretch, dead lines, ~22% slowdown)
	*(vu32*)0x40000B8 = 1 | (0xA240u<<16);
	*(vu16*)0x40000C6 = 0;
	if(wenable)
	{
		*(vu32*)0x40000BC = (u32)&sc_vtab1[1];
		*(vu32*)0x40000C0 = 0x04000016;
		*(vu32*)0x40000C4 = 1 | (0xA240u<<16);
	}
	*(vu16*)0x40000D2 = 0;

	{	// palette (entries 1..4 of BG palette 8)
		vu16 *pal=(vu16*)0x05000100;
		u16 *src=(u16*)gbc_palette;
		pal[1]=src[0]; pal[2]=src[1]; pal[3]=src[2]; pal[4]=src[3];
	}

	// ---- PHASE B: budgeted build work (may run past vblank safely) ----
	sc_budget = sc_menu_build ? 0x7FFFFFFF : 64;
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
	blank = BLANK_TILE | (BG_PAL<<12);
	if(((lcdc ^ sc_last_lcdc) & 0x58) || sc_wst_cur != sc_last_wy)
	{	// v26: NO full wipe on window change - the old entries stay
		// displayed until each row's replacement is built (same rule as
		// BG rows). Only rows ABOVE the new window top get blanked (they
		// left the window and must be transparent).
		int i, lim = wenable ? wtop : 18;
		vu16 *m=WIN_MAP;
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

	// ---- BG0 ring viewport ----
	row0 = scy>>3;
	for(r=0;r<VIS_ROWS;r++)
	{
		int mr=(row0+r)&31;
		const u8 *mrow;
		u16 st;
		if(wenable && r>wtop+1) continue;
		st = sc_row_c0[mr];
		if(mdirty[mr]) { st=0xFFFF; sc_row_c0[mr]=0xFFFF; }
		// (persist the invalidation NOW: if the rebuild below runs out of
		// budget, the row must stay invalid for next frame - v26)
		if(st==(u16)C0)
		{	// valid: rotate eviction stamps (1/8 of rows per frame)
			if(((mr^sc_gen)&7)==0)
				for(j=0;j<ncontent;j++) sc_stamp_slot(mr,C0+j);
			continue;
		}
		// rebuilding: the row's OLD cells are still on screen this frame -
		// stamp them every frame or a slow convergence gets its displayed
		// cells evicted underneath it (menu-churn garbage loop)
		for(j=0;j<ncontent;j++) sc_stamp_slot(mr,C0+j);
		mrow = gbmap + mr*32;
		if(st!=0xFFFF && (st==(u16)(C0-1) || st==(u16)(C0+1)))
		// (st==0xFFFF must never classify as a pan: at C0==0 the invalid
		// sentinel equals (u16)(C0-1) - v26 sentinel-collision fix)
		{	// panning: build only the entering column
			int A = (st==(u16)(C0-1)) ? C0+ncontent-1 : C0;
			u16 e = sc_cell_entry(mrow,A,mode8000,ccols);
			if(!e) continue;               // budget: retry next frame
			sc_map_put(mr,A,e);
		}
		else
		{	// teleport/new row: full rebuild (budget-gated)
			int complete=1;
			for(j=0;j<ncontent;j++)
			{
				u16 e = sc_cell_entry(mrow,C0+j,mode8000,ccols);
				if(!e) { complete=0; continue; }
				sc_map_put(mr,C0+j,e);
			}
			if(!complete) continue;
		}
		for(j=1;j<=guardL;j++) sc_map_put(mr,C0-j,blank);
		for(j=0;j<guardR;j++)  sc_map_put(mr,C0+ncontent+j,blank);
		sc_row_c0[mr]=(u16)C0;
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

	*(vu16*)0x05000000 = 0;        // backdrop black (borders)
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
