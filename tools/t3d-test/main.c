// Tiny3D feasibility test: CI8 + TLUT texture, z-buffer, back-face culling, fog, alpha blend, vertex colours.
#include <libdragon.h>
#include <t3d/t3d.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

static uint16_t pal[256] __attribute__((aligned(16)));
static uint8_t tex[32 * 32] __attribute__((aligned(16)));

#define NV 64
static T3DVertPacked* vb;
static int nv;

static void setv(int i, float x, float y, float z, float u, float v, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int16_t* p = t3d_vertbuffer_get_pos(vb, i);
    p[0] = (int16_t)x; p[1] = (int16_t)y; p[2] = (int16_t)z;
    int16_t* st = t3d_vertbuffer_get_uv(vb, i);
    st[0] = (int16_t)(u * 32); st[1] = (int16_t)(v * 32);
    uint8_t* c = t3d_vertbuffer_get_rgba(vb, i);
    c[0] = r; c[1] = g; c[2] = b; c[3] = a;
}

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    t3d_init((T3DInitParams){});
    T3DViewport vp = t3d_viewport_create();
    surface_t zbuf = surface_alloc(FMT_RGBA16, 320, 240);

    // palette: 256 entries, texture = diagonal gradient with checker
    for (int i = 0; i < 256; i++) {
        int r = i, g = (i * 3) & 255, b = 255 - i;
        pal[i] = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
    }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            tex[y * 32 + x] = (((x >> 3) ^ (y >> 3)) & 1) ? (uint8_t)(x * 4 + y) : (uint8_t)(255 - x * 4);

    surface_t ts = surface_make(tex, FMT_CI8, 32, 32, 32);
    data_cache_hit_writeback(tex, sizeof(tex));
    data_cache_hit_writeback(pal, sizeof(pal));

    vb = malloc_uncached(sizeof(T3DVertPacked) * NV / 2);
    memset(vb, 0, sizeof(T3DVertPacked) * NV / 2);
    // ground quad (CCW seen from above, y up), 0..3
    setv(0, -100, 0, -100, 0, 0, 255, 255, 255, 255);
    setv(1, 100, 0, -100, 64, 0, 255, 255, 255, 255);
    setv(2, 100, 0, 100, 64, 64, 128, 128, 128, 255);
    setv(3, -100, 0, 100, 0, 64, 128, 128, 128, 255);
    // wall upright, front facing +z (CCW seen from +z), 4..7
    setv(4, -30, 0, 0, 0, 0, 255, 255, 255, 255);
    setv(5, 30, 0, 0, 32, 0, 255, 255, 255, 255);
    setv(6, 30, 60, 0, 32, 32, 255, 255, 255, 255);
    setv(7, -30, 60, 0, 0, 32, 255, 255, 255, 255);
    // same wall but wound clockwise (should be culled), shifted: 8..11
    setv(8, 40, 0, 0, 0, 0, 255, 255, 255, 255);
    setv(9, 40, 60, 0, 0, 32, 255, 255, 255, 255);
    setv(10, 100, 60, 0, 32, 32, 255, 255, 255, 255);
    setv(11, 100, 0, 0, 32, 0, 255, 255, 255, 255);
    // translucent quad in front, 12..15 (alpha 128 via vertex)
    setv(12, -60, 5, 30, 0, 0, 255, 255, 255, 128);
    setv(13, 0, 5, 30, 32, 0, 255, 255, 255, 128);
    setv(14, 0, 40, 30, 32, 32, 255, 255, 255, 128);
    setv(15, -60, 40, 30, 0, 32, 255, 255, 255, 128);
    data_cache_hit_writeback(vb, sizeof(T3DVertPacked) * NV / 2);

    T3DMat4FP* mat = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4fp_identity(mat);

    float ang = 0.0f;
    for (int frame = 0;; frame++) {
        surface_t* fb = display_get();
        rdpq_attach(fb, &zbuf);
        t3d_frame_start();
        t3d_viewport_attach(&vp);
        t3d_screen_clear_color(RGBA32(20, 20, 60, 255));
        t3d_screen_clear_depth();
        t3d_viewport_set_projection(&vp, T3D_DEG_TO_RAD(60.0f), 5.0f, 600.0f);
        T3DVec3 eye = {{sinf(ang) * 40, 70, 160}}, tgt = {{0, 10, 0}}, up = {{0, 1, 0}};
        ang += 0.0f;
        t3d_viewport_look_at(&vp, &eye, &tgt, &up);

        t3d_light_set_count(0);
        t3d_light_set_ambient((uint8_t[]){255, 255, 255, 255});
        t3d_fog_set_range(100.0f, 400.0f);
        t3d_fog_set_enabled(true);
        rdpq_set_fog_color(RGBA32(20, 20, 60, 255));

        // opaque textured, fog
        rdpq_mode_begin();
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, SHADE, ZERO), (ZERO, ZERO, ZERO, ONE)));
        rdpq_mode_tlut(TLUT_RGBA16);
        rdpq_mode_filter(FILTER_BILINEAR);
        rdpq_mode_persp(true);
        rdpq_mode_zbuf(true, true);
        rdpq_mode_fog(RDPQ_FOG_STANDARD);
        rdpq_mode_blender(0);
        rdpq_mode_end();
        rdpq_tex_upload_tlut(pal, 0, 256);
        rdpq_tex_upload(TILE0, &ts, &(rdpq_texparms_t){ .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE });

        t3d_state_set_drawflags(T3D_FLAG_DEPTH | T3D_FLAG_TEXTURED | T3D_FLAG_SHADED | T3D_FLAG_CULL_BACK);
        t3d_matrix_push(mat);
        t3d_vert_load(vb, 0, 16);
        t3d_tri_draw(0, 1, 2); t3d_tri_draw(0, 2, 3);
        t3d_tri_draw(4, 5, 6); t3d_tri_draw(4, 6, 7);
        t3d_tri_draw(8, 9, 10); t3d_tri_draw(8, 10, 11);
        t3d_tri_sync();

        // blended: no fog, alpha from shade alpha
        rdpq_mode_begin();
        rdpq_mode_fog(0);
        rdpq_mode_blender(RDPQ_BLENDER_MULTIPLY);
        rdpq_mode_zbuf(true, false);
        rdpq_mode_combiner(RDPQ_COMBINER1((TEX0, ZERO, SHADE, ZERO), (ZERO, ZERO, ZERO, SHADE)));
        rdpq_mode_end();
        t3d_fog_set_enabled(false);
        t3d_tri_draw(12, 13, 14); t3d_tri_draw(12, 14, 15);
        t3d_tri_sync();
        t3d_matrix_pop(1);

        rdpq_detach_show();
    }
}
