// N64 RDP back-end for the BRender triangle rasteriser.
//
// BRender (lib/BRender-v1.3.2/drivers/pentprim/rdprend.c) calls the N64_Rdp_* functions below.
// The 3D scene is drawn by the RDP (textured, perspective-correct, z-buffered, bilinear) into a
// private RGBA5551 surface; at the end of the scene every covered pixel is marked "transparent"
// (index 0) in the game's 8-bit buffer, so that the 2D layer (HUD, cockpit, sky outside the
// 3D geometry...) keeps being drawn by the CPU on top. N64_Rdp_Compose() then merges both in Swap().
#include <libdragon.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
extern volatile int gN64_stage;
extern void OS_N64_ProfBegin(int);
extern void OS_N64_ProfEnd(int);

int gN64_use_rdp = 0; // boot-menu option "Renderer"

void* gN64_screen_base = NULL; // pixels of the game's back screen (set by Swap)
int gN64_screen_w = 0;
int gN64_screen_h = 0;
int gN64_screen_stride = 0;

static int bb_x0, bb_y0, bb_x1, bb_y1;
static surface_t rdp_color, rdp_zbuf;
static int rdp_surfaces_ok = 0;
static int rdp_attached = 0;   // rdpq_attach is active
static int rdp_cleared = 0;    // colour+z were cleared this frame
static int rdp_have_frame = 0; // something was drawn this frame (compose needed)
static int rdp_first_ox, rdp_first_oy, rdp_first_w, rdp_first_h;
static int rdp_ox, rdp_oy;     // position of the current scene in the back screen
static uint16_t rdp_tlut[256] __attribute__((aligned(8)));
static int rdp_tlut_dirty = 1;
static const uint16_t* rdp_tlut_cur;
static unsigned rdp_pal_serial = 1;

// distance fog, set each frame by the game (depth.c): fog starts at depth z0 (0..1) and reaches strength k
int gN64_fog_on;
float gN64_fog_z0 = 2.0f, gN64_fog_k;
uint8_t gN64_fog_rgb[3];

unsigned N64_Rdp_PaletteSerial(void) {
    return rdp_pal_serial;
}
static const void* rdp_tex_ptr = NULL;
static int rdp_tex_w, rdp_tex_h;
static int rdp_mode = -1;
static uint8_t rdp_bounce[2048] __attribute__((aligned(16)));
static int rdp_bad_tex_logged = 0;

int gN64_rdp_tris = 0; // statistics: triangles sent to the RDP in the last frame

#include <stdio.h>
static int rdp_log_budget = 200;
#define RDPLOG(...) do { if (rdp_log_budget > 0) { rdp_log_budget--; fprintf(stderr, "rdp: " __VA_ARGS__); fputc(10, stderr); } } while (0)
static int rdp_bounce_count = 0;
static uint8_t* rdp_snap = NULL; // 8-bit viewport as it was when the scene started (to detect software-drawn pixels)
static int rdp_snap_w, rdp_snap_h;

#define SCREEN_W 320
#define SCREEN_H 200

void N64_Rdp_PaletteChanged(const uint16_t* pal5551) {
    int i;
    for (i = 0; i < 256; i++) {
        rdp_tlut[i] = pal5551[i];
    }
    rdp_tlut[0] &= ~1; // colour index 0 is transparent in textured primitives
    rdp_tlut_dirty = 1;
    rdp_pal_serial++;
}

uint16_t N64_Rdp_PaletteEntry(int i) {
    return rdp_tlut[i & 255] | 1;
}

static int rdp_alloc_surfaces(void) {
    if (rdp_surfaces_ok) {
        return 1;
    }
    rdp_color = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
    rdp_zbuf = surface_alloc(FMT_RGBA16, SCREEN_W, SCREEN_H);
    if (rdp_color.buffer == NULL || rdp_zbuf.buffer == NULL) {
        if (rdp_color.buffer != NULL) {
            surface_free(&rdp_color);
        }
        if (rdp_zbuf.buffer != NULL) {
            surface_free(&rdp_zbuf);
        }
        return 0;
    }
    rdp_surfaces_ok = 1;
    return 1;
}

static void rdp_base_mode(void) {
    rdpq_set_mode_standard();
    rdpq_mode_zbuf(true, true);
    rdpq_mode_persp(true);
    rdpq_mode_filter(FILTER_BILINEAR);
    rdpq_mode_dithering(DITHER_BAYER_BAYER);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdp_mode = -1;
    rdp_tex_ptr = NULL;
}

// Called at the start of each 3D scene. Returns 1 if the RDP will render this scene.
static uint8_t blend_tiles[13 * 20];
static uint8_t* rdp_pix;
static int rdp_pix_row, rdp_pix_w, rdp_pix_h;
static struct { uint8_t* pix; int row, w, h, ox, oy; } rdp_views[2];
static int rdp_view_next;

// Before a blended triangle is drawn: the RDP layer must hold the software image under it, otherwise the blend
// mixes with black. Tiles of RDP-uncovered pixels (alpha bit 0) get the 8-bit image's colour (alpha stays 0).
void N64_Rdp_PrepareBlend(float minx, float miny, float maxx, float maxy) {
    int tx0 = (int)minx >> 4, ty0 = (int)miny >> 4, tx1 = (int)maxx >> 4, ty1 = (int)maxy >> 4;
    int tx, ty, waited = 0;
    if (!rdp_attached || rdp_pix == NULL) {
        return;
    }
    if (tx0 < 0) tx0 = 0;
    if (ty0 < 0) ty0 = 0;
    if (tx1 > 19) tx1 = 19;
    if (ty1 > 12) ty1 = 12;
    for (ty = ty0; ty <= ty1; ty++) {
        for (tx = tx0; tx <= tx1; tx++) {
            int x, y, sx0, sy0;
            if (blend_tiles[ty * 20 + tx]) {
                continue;
            }
            blend_tiles[ty * 20 + tx] = 1;
            if (!waited) {
                gN64_stage = 14;
                rspq_wait();
                gN64_stage = 15;
                waited = 1;
            }
            sx0 = tx * 16;
            sy0 = ty * 16;
            for (y = sy0; y < sy0 + 16 && y < SCREEN_H; y++) {
                int py = y - rdp_oy;
                uint16_t* d;
                if (py < 0 || py >= rdp_pix_h) continue;
                d = (uint16_t*)((uint8_t*)UncachedAddr(rdp_color.buffer) + (size_t)y * rdp_color.stride);
                for (x = sx0; x < sx0 + 16 && x < SCREEN_W; x++) {
                    int px = x - rdp_ox;
                    if (px < 0 || px >= rdp_pix_w) continue;
                    if (!(d[x] & 1)) {
                        d[x] = rdp_tlut[rdp_pix[(size_t)py * rdp_pix_row + px]] & ~1;
                    }
                }
            }
        }
    }
}

int N64_Rdp_BeginScene(void* pixels, int row_bytes, int w, int h) {
    long off;
    int ox, oy;

    if (!gN64_use_rdp || gN64_screen_base == NULL || gN64_screen_w != SCREEN_W || gN64_screen_h != SCREEN_H) {
        return 0;
    }
    if (row_bytes != gN64_screen_stride) {
        return 0;
    }
    off = (long)((uint8_t*)pixels - (uint8_t*)gN64_screen_base);
    if (off < 0 || off >= (long)gN64_screen_stride * SCREEN_H) {
        return 0;
    }
    ox = (int)(off % gN64_screen_stride);
    oy = (int)(off / gN64_screen_stride);
    if (ox + w > SCREEN_W || oy + h > SCREEN_H) {
        // the scene is shifted (screen wobble): clip it to what is visible
        if (ox >= SCREEN_W || oy >= SCREEN_H) {
            return 0;
        }
    }
    if (rdp_have_frame) {
        // only the first scene rectangle of a frame goes to the RDP (the rear-view mirror stays in software)
        if (ox != rdp_first_ox || oy != rdp_first_oy || w != rdp_first_w || h != rdp_first_h) {
            return 0;
        }
    } else {
        rdp_first_ox = ox;
        rdp_first_oy = oy;
        rdp_first_w = w;
        rdp_first_h = h;
    }
    if (!rdp_alloc_surfaces()) {
        return 0;
    }
    if (!rdp_attached) {
        if (rdp_snap == NULL) {
            rdp_snap = (uint8_t*)malloc(SCREEN_W * SCREEN_H);
        }
        if (rdp_snap != NULL) {
            int yy;
            rdp_snap_w = w;
            rdp_snap_h = h;
            for (yy = 0; yy < h; yy++) {
                memcpy(rdp_snap + (size_t)yy * w, (uint8_t*)pixels + (size_t)yy * row_bytes, w);
            }
        }
        if (!rdp_cleared) {
            rdpq_attach(&rdp_color, &rdp_zbuf);
            // colour cleared to 0x0000 (alpha bit 0 = "not covered"), z to the far plane
            rdpq_clear(RGBA32(0, 0, 0, 0));
            rdpq_clear_z(0xFFFC);
            rdp_cleared = 1;
        } else {
            rdpq_attach(&rdp_color, &rdp_zbuf);
        }
        rdp_attached = 1;
        bb_x0 = 100000; bb_y0 = 100000; bb_x1 = -100000; bb_y1 = -100000;
        memset(blend_tiles, 0, sizeof(blend_tiles));
        rdp_base_mode();
        if (gN64_fog_on) {
            rdpq_set_fog_color(RGBA32(gN64_fog_rgb[0], gN64_fog_rgb[1], gN64_fog_rgb[2], 255));
        }
        rdp_tlut_dirty = 1;
    }
    if (rdp_tlut_dirty) {
        data_cache_hit_writeback(rdp_tlut, sizeof(rdp_tlut));
        rdpq_tex_upload_tlut(rdp_tlut, 0, 256);
        rdp_tlut_cur = rdp_tlut;
        rdp_tlut_dirty = 0;
    }
    rdp_ox = ox;
    rdp_oy = oy;
    {
        int si, slot = -1;
        for (si = 0; si < 2; si++) {
            if (rdp_views[si].pix == (uint8_t*)pixels) slot = si;
        }
        if (slot < 0) slot = rdp_view_next ^= 1;
        rdp_views[slot].pix = (uint8_t*)pixels;
        rdp_views[slot].row = row_bytes;
        rdp_views[slot].w = w;
        rdp_views[slot].h = h;
        rdp_views[slot].ox = ox;
        rdp_views[slot].oy = oy;
    }
    rdp_pix = (uint8_t*)pixels;
    rdp_pix_row = row_bytes;
    rdp_pix_w = w;
    rdp_pix_h = h;
    {
        int x1 = ox + w, y1 = oy + h;
        if (x1 > SCREEN_W) {
            x1 = SCREEN_W;
        }
        if (y1 > SCREEN_H) {
            y1 = SCREEN_H;
        }
        rdpq_set_scissor(ox < 0 ? 0 : ox, oy < 0 ? 0 : oy, x1, y1);
    }
    rdp_tex_ptr = NULL; // textures may have been modified since the last scene
    rdp_have_frame = 1;
    RDPLOG("begin scene at %d,%d size %dx%d (surfaces %p %p) tris so far %d", ox, oy, w, h, rdp_color.buffer, rdp_zbuf.buffer, gN64_rdp_tris);
    return 1;
}

int N64_Rdp_OffsetX(void) {
    return rdp_ox;
}
int N64_Rdp_OffsetY(void) {
    return rdp_oy;
}

// Make the given 8-bit texture current. Returns 0 if it cannot be used by the RDP.
static int rdp_eff_w, rdp_eff_h;
// The RDP reads texture data later, when it executes the command queue: copies must not be overwritten
// before then, so each upload gets its own slot (the queue is drained when the ring wraps).
#define RING_SLOTS 16
static uint8_t rdp_ring[RING_SLOTS][2048] __attribute__((aligned(64)));
static int rdp_ring_next;

static uint8_t* ring_slot(void) {
    if (rdp_ring_next >= RING_SLOTS) {
        gN64_stage = 16;
        rspq_wait();
        gN64_stage = 17;
        rdp_ring_next = 0;
    }
    return rdp_ring[rdp_ring_next++];
}

static int pow2_floor(int v) {
    int p = 1;
    while (p * 2 <= v) p *= 2;
    return p;
}

// Uploads the 8-bit texture; textures TMEM cannot take as they are (not a power of two, too big) are
// resampled (nearest). *ew/*eh receive the size actually uploaded.
static int rdp_set_texture(const uint8_t* px, int w, int h, int row_bytes, int* ew, int* eh);
int N64_Rdp_SetTexture(const uint8_t* px, int w, int h, int row_bytes, int* ew, int* eh) {
    int r;
    OS_N64_ProfBegin(9);
    r = rdp_set_texture(px, w, h, row_bytes, ew, eh);
    OS_N64_ProfEnd(9);
    return r;
}

static int rdp_set_texture(const uint8_t* px, int w, int h, int row_bytes, int* ew, int* eh) {
    const uint8_t* src = px;
    int stride = row_bytes;
    int tw = w, th = h;

    if (px == rdp_tex_ptr && w == rdp_tex_w && h == rdp_tex_h) {
        *ew = rdp_eff_w;
        *eh = rdp_eff_h;
        return 1;
    }
    if (w < 1 || h < 1) {
        return 0;
    }
    if ((w & (w - 1)) || (h & (h - 1)) || w * h > 2048 || w > 256 || h > 256 || w < 8) {
        int x, y;
        uint8_t* slot = ring_slot();
        tw = pow2_floor(w > 256 ? 256 : w);
        th = pow2_floor(h > 256 ? 256 : h);
        if (tw < 8) tw = 8;
        while (tw * th > 2048) {
            if (th > tw && th > 1) th /= 2;
            else if (tw > 8) tw /= 2;
            else th /= 2;
        }
        for (y = 0; y < th; y++) {
            const uint8_t* row = px + (size_t)((y * h) / th) * row_bytes;
            for (x = 0; x < tw; x++) {
                slot[y * tw + x] = row[(x * w) / tw];
            }
        }
        src = slot;
        stride = tw;
        rdp_bounce_count++;
    } else if (((uintptr_t)px & 7) || (row_bytes & 7)) {
        int y;
        uint8_t* slot = ring_slot();
        for (y = 0; y < h; y++) {
            memcpy(slot + y * w, px + y * row_bytes, w);
        }
        src = slot;
        stride = w;
        rdp_bounce_count++;
    }
    data_cache_hit_writeback((void*)src, stride * th);
    {
        surface_t s = surface_make((void*)src, FMT_CI8, tw, th, stride);
        rdpq_texparms_t tp = { .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE };
        rdpq_tex_upload(TILE0, &s, &tp);
    }
    rdp_tex_ptr = px;
    rdp_tex_w = w;
    rdp_tex_h = h;
    rdp_eff_w = *ew = tw;
    rdp_eff_h = *eh = th;
    return 1;
}

// Selects the 256-entry palette the RDP uses for the following textured triangles (NULL = the game palette).
void N64_Rdp_UseTlut(const uint16_t* t) {
    if (t == NULL) {
        t = rdp_tlut;
    }
    if (t == rdp_tlut_cur) {
        return;
    }
    data_cache_hit_writeback((void*)t, 512);
    rdpq_tex_upload_tlut(t, 0, 256);
    rdp_tlut_cur = t;
}

void N64_Rdp_InvalidateTlut(void) {
    rdp_tlut_cur = NULL;
}

// kind: 0 flat, 1 textured, 2 flat blended, 3 textured blended
void N64_Rdp_SetMode(int kind) {
    if (kind == rdp_mode) {
        return;
    }
    OS_N64_ProfBegin(17);
    rdp_mode = kind;
    rdpq_mode_begin();
    switch (kind) {
    case 0:
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_tlut(TLUT_NONE);
        rdpq_mode_alphacompare(0);
        rdpq_mode_blender(0);
        rdpq_mode_fog(gN64_fog_on ? RDPQ_FOG_STANDARD : 0);
        rdpq_mode_zbuf(true, true);
        break;
    case 1:
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, SHADE, ZERO), (ZERO, ZERO, ZERO, TEX0)));
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_alphacompare(128);
        rdpq_mode_blender(0);
        rdpq_mode_fog(gN64_fog_on ? RDPQ_FOG_STANDARD : 0);
        rdpq_mode_zbuf(true, true);
        break;
    case 2:
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_tlut(TLUT_NONE);
        rdpq_mode_alphacompare(0);
        rdpq_mode_fog(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        break;
    case 4: // blended, grey palette: intensity + alpha straight from the (special) TLUT
        rdpq_mode_combiner(RDPQ_COMBINER1((ZERO, ZERO, ZERO, TEX0), (ZERO, ZERO, ZERO, TEX0)));
        rdpq_mode_tlut(TLUT_IA16);
        rdpq_mode_alphacompare(1);
        rdpq_mode_fog(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        break;
    default:
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, SHADE, ZERO), (TEX0, ZERO, SHADE, ZERO)));
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_alphacompare(8);
        rdpq_mode_fog(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        break;
    }
    rdpq_mode_end();
    OS_N64_ProfEnd(17);
}

// v: x,y,z,r,g,b,a[,s,t,invw]
static void bb_add(const float* a, const float* b, const float* c) {
    float xs[3] = { a[0], b[0], c[0] }, ys[3] = { a[1], b[1], c[1] };
    for (int i = 0; i < 3; i++) {
        int x0 = (int)xs[i] - 1, x1 = (int)xs[i] + 2, y0 = (int)ys[i] - 1, y1 = (int)ys[i] + 2;
        if (x0 < bb_x0) bb_x0 = x0;
        if (x1 > bb_x1) bb_x1 = x1;
        if (y0 < bb_y0) bb_y0 = y0;
        if (y1 > bb_y1) bb_y1 = y1;
    }
}

void N64_Rdp_TriFlat(const float* a, const float* b, const float* c) {
    gN64_stage = 19;
    bb_add(a, b, c);
    OS_N64_ProfBegin(16);
    rdpq_triangle(&TRIFMT_ZBUF_SHADE, a, b, c);
    OS_N64_ProfEnd(16);
    gN64_rdp_tris++;
}

void N64_Rdp_TriTex(const float* a, const float* b, const float* c) {
    gN64_stage = 18;
    bb_add(a, b, c);
    OS_N64_ProfBegin(16);
    rdpq_triangle(&TRIFMT_ZBUF_SHADE_TEX, a, b, c);
    OS_N64_ProfEnd(16);
    gN64_rdp_tris++;
}

// End of a 3D scene: wait for the RDP, then mark every covered pixel as index 0 in the 8-bit buffer.
void N64_Rdp_EndScene(uint8_t* pixels, int row_bytes, int w, int h) {
    int x, y;
    if (!rdp_attached) {
        return;
    }
    {
        volatile uint32_t canary[16];
        int ci;
        for (ci = 0; ci < 16; ci++) {
            canary[ci] = 0xC0DE0000u + ci;
        }
        RDPLOG("end scene: %d tris, %d bounced textures; waiting for RDP (sp~%p)", gN64_rdp_tris, rdp_bounce_count, (void*)canary);
        gN64_stage = 12;
        rdpq_detach_wait();
        gN64_stage = 13;
        rdp_attached = 0;
        rdp_ring_next = 0;
        for (ci = 0; ci < 16; ci++) {
            if (canary[ci] != 0xC0DE0000u + ci) {
                RDPLOG("STACK CORRUPTED by detach/wait: canary[%d]=%08x", ci, (unsigned)canary[ci]);
                break;
            }
        }
        RDPLOG("end scene: RDP done");
    }
    // the surface lives in uncached RAM (UncachedAddr below): no cache maintenance needed

    {
        int y0 = bb_y0 - rdp_oy, y1 = bb_y1 - rdp_oy, x0 = bb_x0 - rdp_ox, x1 = bb_x1 - rdp_ox;
        if (y0 < 0) y0 = 0;
        if (x0 < 0) x0 = 0;
        if (y1 > h) y1 = h;
        if (x1 > w) x1 = w;
        x0 &= ~1; // even start: 32-bit reads stay aligned
        for (y = y0; y < y1; y++) {
            int sy = rdp_oy + y;
            const uint16_t* src;
            uint8_t* dst = pixels + (size_t)y * row_bytes;
            const uint8_t* old = NULL;
            int n = x1;
            if (sy < 0 || sy >= SCREEN_H) {
                continue;
            }
            if (rdp_ox + n > SCREEN_W) {
                n = SCREEN_W - rdp_ox;
            }
            {
                // read through the cache (burst fills): drop whatever stale lines cover this row segment first
                uintptr_t a0 = ((uintptr_t)rdp_color.buffer + (size_t)sy * rdp_color.stride + (size_t)(rdp_ox + x0) * 2) & ~(uintptr_t)15;
                uintptr_t a1 = ((uintptr_t)rdp_color.buffer + (size_t)sy * rdp_color.stride + (size_t)(rdp_ox + n) * 2 + 15) & ~(uintptr_t)15;
                data_cache_hit_invalidate((void*)a0, a1 - a0);
            }
            src = (const uint16_t*)((uint8_t*)rdp_color.buffer + (size_t)sy * rdp_color.stride) + rdp_ox;
            if (rdp_snap != NULL && w == rdp_snap_w && h == rdp_snap_h) {
                old = rdp_snap + (size_t)y * w;
            }
            x = x0;
            if ((rdp_ox & 1) == 0) {
                const uint32_t* s32 = (const uint32_t*)(src + x0);
                for (; x + 1 < n; x += 2) {
                    uint32_t v = *s32++;
                    if (v & 0x00010001u) {
                        if ((v & 0x00010000u) && (old == NULL || dst[x] == old[x])) dst[x] = 0;
                        if ((v & 1u) && (old == NULL || dst[x + 1] == old[x + 1])) dst[x + 1] = 0;
                    }
                }
            }
            // pixels changed by software drawing during the scene (sprites, smoke...) stay on top
            for (; x < n; x++) {
                if ((src[x] & 1) && (old == NULL || dst[x] == old[x])) {
                    dst[x] = 0;
                }
            }
        }
    }
}

// Swap(): is there 3D to merge this frame?
int N64_Rdp_FrameActive(void) {
    if (rdp_have_frame && rdp_surfaces_ok) {
        data_cache_hit_invalidate(rdp_color.buffer, (size_t)rdp_color.stride * rdp_color.height);
        return 1;
    }
    return 0;
}

// Pixel (x,y) of the RDP layer, RGBA5551
const uint16_t* N64_Rdp_Row(int y) {
    // cached view: the whole layer was invalidated by N64_Rdp_FrameActive
    return (const uint16_t*)((uint8_t*)rdp_color.buffer + (size_t)y * rdp_color.stride);
}

void N64_Rdp_FrameDone(void) {
    if (rdp_attached || gN64_rdp_tris) {
        RDPLOG("frame done (attached %d, tris %d)", rdp_attached, gN64_rdp_tris);
    }
    gN64_rdp_tris = 0;
    if (rdp_attached) {
        rdpq_detach_wait();
        rdp_attached = 0;
    }
    rdp_have_frame = 0;
    rdp_cleared = 0;
}

// ---- smoke on the RDP layer ----
// The game's smoke (SmokeLine) shades the 8-bit screen through a shade table, but pixels drawn by the RDP live
// in the RGBA16 layer (the 8-bit screen holds 0 there). For those pixels the table row is fitted as
// out = in * (1 - k) + T * k and applied to the RGBA16 colour, with the depth test done on the RDP z buffer.
typedef struct {
    const uint8_t* tab;
    unsigned serial;
    uint8_t valid[32];
    float k[32];
    float T[32][3];
} smoke_fit;
static smoke_fit smoke_fits[4];
static int smoke_fit_next;
static int smoke_dB = -1, smoke_dW;
static unsigned smoke_pal_serial;

static int smoke_luma(uint16_t c) {
    return ((c >> 11) & 31) * 77 + ((c >> 6) & 31) * 150 + ((c >> 1) & 31) * 29;
}

static void smoke_rgb(uint16_t c, float* o) {
    o[0] = (float)((c >> 11) & 31);
    o[1] = (float)((c >> 6) & 31);
    o[2] = (float)((c >> 1) & 31);
}

int N64_Rdp_Smoke(uint8_t* scr, const uint8_t* shade, int row, int zs) {
    int vi, off = 0, x, y;
    uint16_t* dst;
    const uint16_t* zp;
    smoke_fit* F = NULL;
    uint16_t c;
    float rgb[3];
    int n;

    if (!rdp_surfaces_ok || row < 0 || row >= 32) {
        return 0;
    }
    for (vi = 0; vi < 2; vi++) {
        if (rdp_views[vi].pix != NULL && scr >= rdp_views[vi].pix && scr < rdp_views[vi].pix + (size_t)rdp_views[vi].h * rdp_views[vi].row) {
            break;
        }
    }
    if (vi == 2) {
        return 0;
    }
    off = (int)(scr - rdp_views[vi].pix);
    y = off / rdp_views[vi].row;
    x = off - y * rdp_views[vi].row;
    if (x >= rdp_views[vi].w) {
        return 0;
    }
    x += rdp_views[vi].ox;
    y += rdp_views[vi].oy;
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) {
        return 0;
    }
    dst = (uint16_t*)((uint8_t*)UncachedAddr(rdp_color.buffer) + (size_t)y * rdp_color.stride) + x;
    c = *dst;
    if (!(c & 1) || *scr != 0) {
        return 0; // not an RDP pixel: software path
    }
    zp = (const uint16_t*)((uint8_t*)UncachedAddr(rdp_zbuf.buffer) + (size_t)y * rdp_zbuf.stride) + x;
    {
        // decompress the RDP z (3-bit exponent, 11-bit mantissa) to our 16-bit depth scale
        static const uint8_t sh[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
        static const uint32_t ad[8] = { 0x00000, 0x20000, 0x30000, 0x38000, 0x3c000, 0x3e000, 0x3f000, 0x3f800 };
        uint16_t v = *zp;
        int e = (v >> 13) & 7;
        uint32_t z18 = ((uint32_t)((v >> 2) & 0x7ff) << sh[e]) + ad[e];
        int z16 = (int)(z18 * (65535.0f / 131068.0f));
        if (z16 <= zs) {
            return 1; // in front of the smoke: untouched
        }
    }
    if (smoke_dB < 0 || smoke_pal_serial != rdp_pal_serial) {
        int i, lmin = 1 << 30, lmax = -1;
        for (i = 0; i < 256; i++) {
            int l = smoke_luma(rdp_tlut[i] | 1);
            if (l < lmin) { lmin = l; smoke_dB = i; }
            if (l > lmax) { lmax = l; smoke_dW = i; }
        }
        smoke_pal_serial = rdp_pal_serial;
        memset(smoke_fits, 0, sizeof(smoke_fits));
    }
    for (n = 0; n < 4; n++) {
        if (smoke_fits[n].tab == shade) {
            F = &smoke_fits[n];
            break;
        }
    }
    if (F == NULL) {
        F = &smoke_fits[smoke_fit_next];
        smoke_fit_next = (smoke_fit_next + 1) & 3;
        memset(F, 0, sizeof(*F));
        F->tab = shade;
    }
    if (!F->valid[row]) {
        float B[3], W[3], oB[3], oW[3], ks = 0;
        int ch, kc = 0;
        smoke_rgb(rdp_tlut[smoke_dB] | 1, B);
        smoke_rgb(rdp_tlut[smoke_dW] | 1, W);
        smoke_rgb(rdp_tlut[shade[row * 256 + smoke_dB]] | 1, oB);
        smoke_rgb(rdp_tlut[shade[row * 256 + smoke_dW]] | 1, oW);
        for (ch = 0; ch < 3; ch++) {
            if (W[ch] - B[ch] > 4.0f) {
                ks += 1.0f - (oW[ch] - oB[ch]) / (W[ch] - B[ch]);
                kc++;
            }
        }
        F->k[row] = kc ? ks / (float)kc : 0.0f;
        if (F->k[row] < 0) F->k[row] = 0;
        if (F->k[row] > 1) F->k[row] = 1;
        for (ch = 0; ch < 3; ch++) {
            float t = F->k[row] > 0.02f ? (oB[ch] - B[ch] * (1.0f - F->k[row])) / F->k[row] : 0;
            F->T[row][ch] = t < 0 ? 0 : (t > 31 ? 31 : t);
        }
        F->valid[row] = 1;
    }
    smoke_rgb(c, rgb);
    {
        float k = F->k[row], r, g, b;
        r = rgb[0] * (1 - k) + F->T[row][0] * k + 0.5f;
        g = rgb[1] * (1 - k) + F->T[row][1] * k + 0.5f;
        b = rgb[2] * (1 - k) + F->T[row][2] * k + 0.5f;
        *dst = (uint16_t)(((int)r << 11) | ((int)g << 6) | ((int)b << 1) | 1);
    }
    return 1;
}
