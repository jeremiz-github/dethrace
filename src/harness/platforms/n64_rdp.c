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
static unsigned rdp_pal_serial = 1;

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
        rdp_base_mode();
        rdp_tlut_dirty = 1;
    }
    if (rdp_tlut_dirty) {
        rdpq_tex_upload_tlut(rdp_tlut, 0, 256);
        rdp_tlut_dirty = 0;
    }
    rdp_ox = ox;
    rdp_oy = oy;
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
int N64_Rdp_SetTexture(const uint8_t* px, int w, int h, int row_bytes) {
    const uint8_t* src = px;
    int stride = row_bytes;

    if (px == rdp_tex_ptr && w == rdp_tex_w && h == rdp_tex_h) {
        return 1;
    }
    if (w < 8 || h < 1 || (w & (w - 1)) || (h & (h - 1)) || w * h > 2048 || w > 256 || h > 256) {
        if (!rdp_bad_tex_logged) {
            rdp_bad_tex_logged = 1;
            debugf("rdp: unusable texture %dx%d\n", w, h);
        }
        return 0;
    }
    if (((uintptr_t)px & 7) || (row_bytes & 7)) {
        int y;
        for (y = 0; y < h; y++) {
            memcpy(rdp_bounce + y * w, px + y * row_bytes, w);
        }
        src = rdp_bounce;
        stride = w;
        rdp_bounce_count++;
    }
    data_cache_hit_writeback((void*)src, stride * h);
    {
        surface_t s = surface_make((void*)src, FMT_CI8, w, h, stride);
        rdpq_texparms_t tp = { .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE };
        rdpq_tex_upload(TILE0, &s, &tp);
    }
    rdp_tex_ptr = px;
    rdp_tex_w = w;
    rdp_tex_h = h;
    return 1;
}

// kind: 0 flat, 1 textured, 2 flat blended, 3 textured blended
void N64_Rdp_SetMode(int kind) {
    if (kind == rdp_mode) {
        return;
    }
    rdp_mode = kind;
    rdpq_mode_begin();
    switch (kind) {
    case 0:
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_tlut(TLUT_NONE);
        rdpq_mode_alphacompare(0);
        rdpq_mode_blender(0);
        rdpq_mode_zbuf(true, true);
        break;
    case 1:
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, SHADE, ZERO), (ZERO, ZERO, ZERO, TEX0)));
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_alphacompare(128);
        rdpq_mode_blender(0);
        rdpq_mode_zbuf(true, true);
        break;
    case 2:
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_tlut(TLUT_NONE);
        rdpq_mode_alphacompare(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        break;
    default:
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, SHADE, ZERO), (ZERO, ZERO, ZERO, SHADE)));
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_alphacompare(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        break;
    }
    rdpq_mode_end();
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
    bb_add(a, b, c);
    rdpq_triangle(&TRIFMT_ZBUF_SHADE, a, b, c);
    gN64_rdp_tris++;
}

void N64_Rdp_TriTex(const float* a, const float* b, const float* c) {
    bb_add(a, b, c);
    rdpq_triangle(&TRIFMT_ZBUF_SHADE_TEX, a, b, c);
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
        rdpq_detach_wait();
        rdp_attached = 0;
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
            src = (const uint16_t*)((uint8_t*)UncachedAddr(rdp_color.buffer) + (size_t)sy * rdp_color.stride) + rdp_ox;
            if (rdp_ox + n > SCREEN_W) {
                n = SCREEN_W - rdp_ox;
            }
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
    return rdp_have_frame && rdp_surfaces_ok;
}

// Pixel (x,y) of the RDP layer, RGBA5551
const uint16_t* N64_Rdp_Row(int y) {
    return (const uint16_t*)((uint8_t*)UncachedAddr(rdp_color.buffer) + (size_t)y * rdp_color.stride);
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
