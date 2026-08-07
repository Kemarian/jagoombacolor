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
EWRAM_BSS static u8  sc_srcsel[9][8];   // per phase: source nibble 0..15
EWRAM_BSS static u8  sc_vdelta[161];
EWRAM_BSS static u16 sc_vtab0[161];
EWRAM_BSS static u16 sc_vtab1[161];

EWRAM_BSS static u8 sc_rowok[32];   // BG map row valid for current viewport
static u16 sc_ncells, sc_tombs, sc_evict_ptr;
static int sc_last_C0=-1, sc_last_scy=-1;
static u8 sc_gen, sc_tables_ok, sc_active, sc_last_lcdc, sc_last_wy;
static u8 sc_N, sc_D;               // current ratio (9/8 or 3/2)
static volatile u8 sc_busy;         // menu-sync vs vblank reentry guard

// Frame work budget: display state must never depend on workload, so cell
// conversions are capped per vblank; unfinished rows/sweeps resume next
// frame (stale content beats torn scanout). Menu toggles build unbounded
// (game paused, overrun invisible).
static int sc_budget;
static u8 sc_menu_build;
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
	for(i=0;i<32;i++) sc_rowok[i]=0;
	sc_ncells=0; sc_tombs=0; sc_evict_ptr=0;
	sc_last_C0=-1; sc_last_scy=-1;
	for(i=0;i<8;i++) bt[i]=0;
	m=SCALED_MAP; for(i=0;i<0x400;i++) m[i]=blank;
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
		if(sc_cell_gen[c]!=sc_gen)
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
	if(c>=MAX_TILES) return BLANK_TILE;
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
	{	// new writes: (re)start the sweep from the top
		VRAM_chr_lastAddr=0xFF;
		sc_sweep_active=1;
		sc_sweep_obj=0;
		sc_sweep_cell=0;
	}
	if(!sc_sweep_active) return;

	// OBJ tiles first (sprite glitches are the most visible staleness)
	while(sc_sweep_obj<128 && sc_budget>0)
	{
		u32 t=sc_sweep_obj*2;
		sc_sweep_obj++;
		if(!TILE_DIRTY(t)) continue;
		sc_convert_obj(XGB_VRAM+t*16,           (u32*)(OBJ_TILES+t*32));
		sc_convert_obj(XGB_VRAM+(t+1)*16,       (u32*)(OBJ_TILES+(t+1)*32));
		sc_convert_obj(XGB_VRAM+0x2000+t*16,    (u32*)(OBJ_TILES+0x2000+t*32));
		sc_convert_obj(XGB_VRAM+0x2000+(t+1)*16,(u32*)(OBJ_TILES+0x2000+(t+1)*32));
		sc_budget--;
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

void scaling_scaled_frame(void)
{
	u8 lcdc;
	const u8 *gbmap, *wmap;
	u8 *wdirty, *mdirty;
	int mode8000,r,j,scy,scx,row0;
	int wenable,wtop,fit;
	int P,C0,phase,slot0,nslots,hbase,ccols;
	u16 dispcnt;

	if(sc_busy)
	{	// mid-build reentry: skip cache work but KEEP the display coherent
		// (re-arm the VOFS DMA from existing tables; a frame without it
		// shows vertically-unscaled content = visible full-frame jump)
		*(vu16*)0x40000BA = 0;
		*(vu32*)0x40000B0 = (u32)&sc_vtab0[1];
		*(vu32*)0x40000B4 = 0x04000012;
		*(vu32*)0x40000B8 = 159 | (0xA240u<<16);
		*(vu16*)0x4000012 = sc_vtab0[0];
		return;
	}
	sc_busy=1;

	// per-frame conversion budget (~64 cells ≈ 30k cycles, fits vblank
	// beside the rest); menu-toggle builds run unbounded (game paused)
	sc_budget = sc_menu_build ? 0x7FFFFFFF : 64;
	sc_menu_build = 0;

	fit = (g_scale_mode==SCALE_FIT);
	sc_N = fit ? 9 : 3;
	sc_D = fit ? 8 : 2;
	if(!sc_tables_ok) { sc_build_tables(); sc_tables_ok=1; }
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
	scy = scrollY; scx = scrollX;
	ccols = (32*sc_N)/sc_D;                    // 36 (fit) / 48 (stretch)
	// layout: fit centers 180px (content in slots 4..27, HOFS base 2);
	// stretch fills (slots 0..30, HOFS base 0)
	slot0  = fit ? 4  : 0;
	nslots = fit ? 24 : 31;
	hbase  = fit ? (32 - 30) : 0;              // slot0*8 - border(30) = 2
	P  = (scx*sc_N)/sc_D;                      // scaled scroll position
	C0 = P>>3;
	phase = P&7;

	// ---- registers, palette, vertical tables, DMA: FIRST ----
	{
		vu16 *pal=(vu16*)0x05000100;
		u16 *src=(u16*)gbc_palette;
		pal[1]=src[0]; pal[2]=src[1]; pal[3]=src[2]; pal[4]=src[3];
		*(vu16*)0x05000000 = 0;
	}
	wenable = (lcdc&0x20) && windowY<144 && windowX<8;
	wtop = windowY>>3;

	REG_BG0CNT = 3 | (1<<2) | (10<<8);         // 32x32 map now
	*(vu16*)0x4000010 = (u16)(hbase + phase);
	if(scy != sc_last_scy)
	{	// vertical VOFS tables depend only on scy: rebuild on change
		int y;
		for(y=0;y<161;y++)
		{
			sc_vtab0[y] = (u16)((scy - sc_vdelta[y]) & 0xFF);
			sc_vtab1[y] = (u16)((0   - sc_vdelta[y]) & 0xFF);
		}
		sc_last_scy = scy;
	}
	*(vu16*)0x4000012 = sc_vtab0[0];
	dispcnt = 0x1140;
	if(wenable)
	{
		*(vu16*)0x400000A = 2 | (1<<2) | (12<<8);
		*(vu16*)0x4000014 = (u16)(fit ? -30 : 0); // window left at border edge
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
	*(vu16*)0x4000000 = dispcnt;
	// arm VOFS HBlank DMAs (disable first: SAD only latches on 0->1)
	*(vu16*)0x40000BA = 0;
	*(vu32*)0x40000B0 = (u32)&sc_vtab0[1];
	*(vu32*)0x40000B4 = 0x04000012;
	*(vu32*)0x40000B8 = 159 | (0xA240u<<16);
	*(vu16*)0x40000C6 = 0;
	if(wenable)
	{
		*(vu32*)0x40000BC = (u32)&sc_vtab1[1];
		*(vu32*)0x40000C0 = 0x04000016;
		*(vu32*)0x40000C4 = 159 | (0xA240u<<16);
	}
	*(vu16*)0x40000D2 = 0;

	// ---- heavy work: dirty reconverts + viewport map build ----
	if(sc_active) sc_update_dirty();

	gbmap  = XGB_VRAM + ((lcdc&0x08) ? 0x1C00 : 0x1800);
	mode8000 = lcdc&0x10;
	mdirty = dirty_map_words + ((lcdc&0x08) ? 32 : 0);
	wmap   = XGB_VRAM + ((lcdc&0x40) ? 0x1C00 : 0x1800);
	wdirty = dirty_map_words + ((lcdc&0x40) ? 32 : 0);
	if(((lcdc ^ sc_last_lcdc) & 0x78) || windowY != sc_last_wy)
	{
		int i;
		vu16 *m=WIN_MAP;
		u16 blank=BLANK_TILE|(BG_PAL<<12);
		for(i=0;i<0x400;i++) m[i]=blank;
		for(i=0;i<18;i++) sc_wbuilt[i]=0;
	}
	sc_last_lcdc=lcdc; sc_last_wy=windowY;

	{	// blank-tile insurance
		u32 *bt=(u32*)(CHARBASE1 + BLANK_TILE*32);
		int i;
		for(i=0;i<8;i++) bt[i]=0;
	}

	if(C0 != sc_last_C0)
	{	// viewport moved: all rows must rebuild
		int i;
		for(i=0;i<32;i++) sc_rowok[i]=0;
		sc_last_C0=C0;
	}
	row0 = scy>>3;
	for(r=0;r<VIS_ROWS;r++)
	{
		int mr=(row0+r)&31;
		const u8 *mrow;
		vu16 *out;
		if(wenable && r>wtop+1) continue;
		out = SCALED_MAP + mr*32;
		if(mdirty[mr]) { mdirty[mr]=0; sc_rowok[mr]=0; }
		if(sc_rowok[mr])
		{	// unchanged: just re-stamp cells so eviction skips them
			for(j=0;j<nslots;j++)
			{
				u32 t=out[slot0+j]&0x3FF;
				if(t>=TILE_BASE && t<TILE_BASE+MAX_TILES)
					sc_cell_gen[t-TILE_BASE]=sc_gen;
			}
			continue;
		}
		mrow = gbmap + mr*32;
		{
			int complete=1;
			for(j=0;j<nslots;j++)
			{
				u16 e = sc_cell_entry(mrow,C0+j,mode8000,ccols);
				if(!e) { complete=0; continue; }  // keep old entry, retry
				out[slot0+j] = e;
			}
			if(complete) sc_rowok[mr]=1;
		}
	}

	if(wenable)
	{
		int wcols = fit ? 23 : 30;        // 180px vs 240px of window
		for(r=wtop;r<18;r++)
		{
			int wr=r-wtop;
			const u8 *mrow = wmap + wr*32;
			vu16 *out = WIN_MAP + r*32;
			if(wdirty[wr]) { wdirty[wr]=0; sc_wbuilt[r]=0; }
			if(sc_wbuilt[r])
			{	// re-stamp so eviction skips these cells
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
	u16 pd = 230;                              // 256*9/10 (both modes)
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
	}
}
