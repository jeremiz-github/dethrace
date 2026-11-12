// Dethrace N64 - Tiny3D (RSP geometry) vs rdpq_triangle (CPU-assembled) benchmark.
// Same random screen-space triangle workload as rdptest, 320x200, 16bpp, z-buffer, bilinear, persp.
//
//   Z         : triangle count 0 / 250 / 500 / 1000 / 2000
//   B         : method   0 = rdpq_triangle (what the game does now)
//                        1 = Tiny3D, unshared vertices (3 per tri, worst case)
//                        2 = Tiny3D, shared vertices (indexed mesh, like real models)
//   A         : bilinear <-> point sampling
// Screen shows: ms/frame, CPU ms spent submitting the triangles, fps.
#include <libdragon.h>
#include <t3d/t3d.h>
#include <math.h>
#include <t3d/t3dmath.h>
#include <stdlib.h>
#include <string.h>

// libdragon turns debug_init_*() into `false` under NDEBUG: call the real functions
#undef debug_init_isviewer
#undef debug_init_emulog
#undef debug_init_usblog
bool debug_init_emulog(void);
bool debug_init_usblog(void);
#define W 320
#define H 200
#define MAXT 2000

static surface_t tex;
typedef struct { float x[3], y[3], z, s[3], t[3]; } rtri;
static rtri rt[MAXT];
typedef struct { float x, y, z, r, g, b, a, s, t, iw; } vtx;

static void make_tex(void) {
    tex = surface_alloc(FMT_RGBA16, 32, 32);
    for (int y = 0; y < 32; y++) {
        uint16_t* row = (uint16_t*)((uint8_t*)tex.buffer + y * tex.stride);
        for (int x = 0; x < 32; x++) {
            int c = ((x >> 2) ^ (y >> 2)) & 1;
            row[x] = c ? 0xCA41 : 0x7B05;
        }
    }
}

static void make_tris(void) {
    srand(1234);
    for (int i = 0; i < MAXT; i++) {
        float cx = rand() % W, cy = rand() % H;
        float size = 10 + rand() % 30;
        for (int k = 0; k < 3; k++) {
            rt[i].x[k] = cx + ((rand() % 200) - 100) * size / 100.0f;
            rt[i].y[k] = cy + ((rand() % 200) - 100) * size / 100.0f;
            rt[i].s[k] = (rand() % 100) / 100.0f;
            rt[i].t[k] = (rand() % 100) / 100.0f;
        }
        rt[i].z = (rand() % 1000) / 1000.0f;
    }
}

// Tiny3D vertex buffers (uncached, 16-byte aligned)
static T3DVertPacked* vb_unshared;   // MAXT*3 vertices
static T3DVertPacked* vb_shared;     // grid vertices
static uint16_t* grid_idx;           // 3 per triangle
static int grid_tris, grid_verts, quad_pair;
#define GW 46                         // grid columns (cells)
#define GH 24

static void set_vertex(T3DVertPacked* vb, int i, float x, float y, float z, float s, float t) {
    int16_t* p = t3d_vertbuffer_get_pos(vb, i);
    p[0] = (int16_t)(x - W * 0.5f); p[1] = (int16_t)(H * 0.5f - y); p[2] = (int16_t)z;
    int16_t* uv = t3d_vertbuffer_get_uv(vb, i);
    uv[0] = (int16_t)(s * 32 * 32); uv[1] = (int16_t)(t * 32 * 32);
    uint8_t* c = t3d_vertbuffer_get_rgba(vb, i);
    c[0] = c[1] = c[2] = 0xE0; c[3] = 0xFF;
}

static void build_t3d(void) {
    vb_unshared = malloc_uncached_aligned(16, sizeof(T3DVertPacked) * ((MAXT * 3 + 1) / 2 + 4));
    for (int i = 0; i < MAXT; i++)
        for (int k = 0; k < 3; k++)
            set_vertex(vb_unshared, i * 3 + k, rt[i].x[k], rt[i].y[k], (rt[i].z - 0.5f) * 40.0f, rt[i].s[k] * 2, rt[i].t[k] * 2);
    // proof-of-life quad (4 vertices, after the random ones): bright, top-left corner, 60x40 px
    {
        int q = MAXT * 3 + (MAXT * 3) % 2;   // keep the quad pair-aligned
        set_vertex(vb_unshared, q + 0, 10, 10, 0, 0, 0);
        set_vertex(vb_unshared, q + 1, 70, 10, 0, 1, 0);
        set_vertex(vb_unshared, q + 2, 70, 50, 0, 1, 1);
        set_vertex(vb_unshared, q + 3, 10, 50, 0, 0, 1);
        quad_pair = q / 2;
    }
    // shared grid: GW x GH cells -> (GW+1)*(GH+1) verts, 2 tris per cell (~2200 tris)
    grid_verts = (GW + 1) * (GH + 1);
    grid_tris = GW * GH * 2;
    vb_shared = malloc_uncached_aligned(16, sizeof(T3DVertPacked) * (grid_verts / 2 + 2));
    for (int j = 0; j <= GH; j++)
        for (int i = 0; i <= GW; i++)
            set_vertex(vb_shared, j * (GW + 1) + i, i * (W / (float)GW), j * (H / (float)GH), (float)(((i * 7 + j * 13) % 50) - 25), i * 0.5f, j * 0.5f);
    grid_idx = malloc(sizeof(uint16_t) * grid_tris * 3);
    int n = 0;
    for (int j = 0; j < GH; j++)
        for (int i = 0; i < GW; i++) {
            uint16_t a = j * (GW + 1) + i, b = a + 1, c = a + (GW + 1), d = c + 1;
            grid_idx[n++] = a; grid_idx[n++] = c; grid_idx[n++] = b;
            grid_idx[n++] = b; grid_idx[n++] = c; grid_idx[n++] = d;
        }
}

static void setup_mode(bool bilinear) {
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
    rdpq_mode_zbuf(true, true);
    rdpq_mode_persp(true);
    rdpq_mode_filter(bilinear ? FILTER_BILINEAR : FILTER_POINT);
    rdpq_mode_antialias(AA_NONE);
    rdpq_tex_upload(TILE0, &(surface_t){ .buffer = tex.buffer, .width = 32, .height = 32, .stride = tex.stride, .flags = FMT_RGBA16 }, NULL);
}

static void draw_rdpq(int n) {
    for (int i = 0; i < n; i++) {
        vtx v[3];
        for (int k = 0; k < 3; k++)
            v[k] = (vtx){ rt[i].x[k], rt[i].y[k], rt[i].z, 0.85f, 0.85f, 0.85f, 1, rt[i].s[k] * 64, rt[i].t[k] * 64, 1.0f };
        rdpq_triangle(&TRIFMT_ZBUF_SHADE_TEX, (float*)&v[0], (float*)&v[1], (float*)&v[2]);
    }
}

static void draw_quad(void) {
    t3d_vert_load(vb_unshared + quad_pair, 0, 4);
    t3d_tri_draw(0, 1, 2);
    t3d_tri_draw(2, 3, 0);
    t3d_tri_draw(2, 1, 0);   // both windings, so culling cannot hide it
    t3d_tri_draw(0, 3, 2);
    t3d_tri_sync();
}

static void draw_t3d_unshared(int n) {
    // 66 vertices per load (22 triangles); an even count keeps every load aligned on a vertex pair
    for (int i = 0; i < n; i += 22) {
        int cnt = n - i < 22 ? n - i : 22;
        t3d_vert_load(vb_unshared + (i * 3) / 2, 0, cnt * 3);
        for (int k = 0; k < cnt; k++) t3d_tri_draw(k * 3, k * 3 + 1, k * 3 + 2);
        t3d_tri_sync();
    }
}

static void draw_t3d_shared(int n) {
    // draw the first n triangles of the grid, row by row; reload vertices per row pair
    int done = 0;
    int tri_per_row = GW * 2;
    for (int j = 0; j < GH && done < n; j++) {
        // vertices of rows j and j+1: 2*(GW+1) = 94 > 70 -> split the row into two halves of 23 cells
        for (int half = 0; half < 2 && done < n; half++) {
            int c0 = half * 23, c1 = half ? GW : 23;
            int cells = c1 - c0;
            int base0 = j * (GW + 1) + c0, base1 = (j + 1) * (GW + 1) + c0;
            // load two runs of (cells+1) vertices: top row at cache 0.., bottom row after it
            // vertex pairs are 2 per T3DVertPacked, so use even start offsets
            int s0 = base0 & ~1, s1 = base1 & ~1;
            int off0 = base0 - s0, off1 = base1 - s1;
            int n0 = cells + 1 + off0, n1 = cells + 1 + off1;
            t3d_vert_load(vb_shared + s0 / 2, 0, n0);
            t3d_vert_load(vb_shared + s1 / 2, n0, n1);
            for (int i = 0; i < cells && done < n; i++) {
                int a = off0 + i, b = a + 1, c = n0 + off1 + i, d = c + 1;
                t3d_tri_draw(a, c, b); done++;
                if (done >= n) break;
                t3d_tri_draw(b, c, d); done++;
            }
            t3d_tri_sync();
        }
    }
    (void)tri_per_row;
}


// ---- depth/screen probe: what z value does the RDP z-buffer hold for Tiny3D vs rdpq_triangle? ----
// Reads the z-buffer back on the CPU at the screen centre. Results go to the log and to probe_txt[] (shown on screen).
#define NPROBE 12
static char probe_txt[NPROBE][64];
static int probe_n;
static uint16_t read_z(surface_t* zb, int x, int y) {
    rspq_wait();
    data_cache_hit_invalidate(zb->buffer, zb->stride * H);
    return ((uint16_t*)((uint8_t*)zb->buffer + y * zb->stride))[x];
}
static void probe_run(surface_t* zb, T3DViewport* vp, T3DMat4FP* mfp, float cam_dist) {
    static const float rz[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    static const float dist[] = { 12.0f, 30.0f, 100.0f, 300.0f, 700.0f, 990.0f };
    T3DVertPacked* vb = malloc_uncached_aligned(16, sizeof(T3DVertPacked) * 4);
    // rdpq_triangle with known z
    for (int i = 0; i < 5; i++) {
        surface_t* disp = display_get();
        rdpq_attach(disp, zb);
        rdpq_clear(RGBA32(0, 0, 0, 255));
        rdpq_clear_z(0xFFFC);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_zbuf(true, true);
        vtx v[3] = { { 100, 60, rz[i], 1, 1, 1, 1, 0, 0, 1 }, { 220, 60, rz[i], 1, 1, 1, 1, 0, 0, 1 }, { 160, 150, rz[i], 1, 1, 1, 1, 0, 0, 1 } };
        rdpq_triangle(&TRIFMT_ZBUF_SHADE, (float*)&v[0], (float*)&v[1], (float*)&v[2]);
        rdpq_detach();
        uint16_t z = read_z(zb, W / 2, H / 2);
        fprintf(stderr, "probe rdpq z=%.2f -> zbuf 0x%04X\n", rz[i], z);
        if (probe_n < NPROBE) snprintf(probe_txt[probe_n++], 64, "rdpq z %.2f -> %04X", rz[i], z);
    }
    // Tiny3D triangle at view distance d (camera looks along +z from z=-cam_dist)
    for (int i = 0; i < 6; i++) {
        float zw = -cam_dist + dist[i];
        float sc = dist[i] / cam_dist;   // keep the on-screen size constant-ish
        int16_t x0 = (int16_t)(-60 * sc), x1 = (int16_t)(60 * sc), y0 = (int16_t)(-40 * sc), y1 = (int16_t)(60 * sc);
        int16_t* p;
        p = t3d_vertbuffer_get_pos(vb, 0); p[0] = x0; p[1] = y0; p[2] = (int16_t)zw;
        p = t3d_vertbuffer_get_pos(vb, 1); p[0] = x1; p[1] = y0; p[2] = (int16_t)zw;
        p = t3d_vertbuffer_get_pos(vb, 2); p[0] = 0;  p[1] = y1; p[2] = (int16_t)zw;
        for (int k = 0; k < 3; k++) { uint8_t* c = t3d_vertbuffer_get_rgba(vb, k); c[0] = c[1] = c[2] = c[3] = 255; }
        surface_t* disp = display_get();
        rdpq_attach(disp, zb);
        rdpq_clear(RGBA32(0, 0, 0, 255));
        rdpq_clear_z(0xFFFC);
        t3d_frame_start();
        t3d_viewport_attach(vp);
        rdpq_set_mode_standard();
        rdpq_mode_combiner(RDPQ_COMBINER_SHADE);
        rdpq_mode_zbuf(true, true);
        rdpq_mode_persp(true);
        t3d_state_set_drawflags(T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
        t3d_light_set_count(0);
        t3d_matrix_push(mfp);
        t3d_vert_load(vb, 0, 3);
        t3d_tri_draw(0, 1, 2);
        t3d_tri_draw(2, 1, 0);
        t3d_tri_sync();
        t3d_matrix_pop(1);
        rdpq_detach();
        uint16_t z = read_z(zb, W / 2, H / 2);
        fprintf(stderr, "probe t3d view dist %.0f (near 10 far 1000) -> zbuf 0x%04X\n", dist[i], z);
        if (probe_n < NPROBE) snprintf(probe_txt[probe_n++], 64, "t3d d %.0f -> %04X", dist[i], z);
    }
    free_uncached(vb);
}

int main(void) {
    debug_init_emulog();
    fprintf(stderr, "t3dtest: boot\n");
#ifndef T3D_AUTO
    debug_init_usblog();
#endif
    display_init((resolution_t){ .width = W, .height = H, .interlaced = false }, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();
    fprintf(stderr, "t3dtest: display ok\n");
    rdpq_init();
    fprintf(stderr, "t3dtest: rdpq ok\n");
    t3d_init((T3DInitParams){});
    fprintf(stderr, "t3dtest: t3d ok\n");
    make_tex();
    make_tris();
    build_t3d();
    fprintf(stderr, "t3dtest: built\n");

    surface_t zb = surface_alloc(FMT_RGBA16, W, H);
    T3DViewport vp = t3d_viewport_create();
    // pixel-space orthographic projection: x 0..W, y 0..H (down), z -near..-far
    // Perspective camera looking at the z=0 plane, exactly like the Tiny3D quad example: 1 unit = 1 screen pixel there.
    const float fov = T3D_DEG_TO_RAD(60.0f);
    const float cam_dist = (W * 0.5f) / tanf(fov * 0.5f);   // so that the plane z=0 spans exactly W pixels
    t3d_viewport_set_projection(&vp, fov, 10.0f, 1000.0f);
    t3d_viewport_look_at(&vp, &(fm_vec3_t){{0, 0, -cam_dist}}, &(fm_vec3_t){{0, 0, 0}}, &(fm_vec3_t){{0, 1, 0}});
    fm_mat4_t mf; fm_mat4_identity(&mf);
    T3DMat4FP* mfp = malloc_uncached(sizeof(T3DMat4FP));
    t3d_mat4_to_fixed(mfp, &mf);
    rdpq_font_t* font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
    rdpq_text_register_font(1, font);

    probe_run(&zb, &vp, mfp, cam_dist);
    bool bilinear = true;
    int method = 0, stress_idx = 3;
    const int stress[] = { 0, 250, 500, 1000, 2000 };
    float ms = 0, cpu_ms = 0, fps = 0;
    uint32_t t_last = get_ticks(), cpu_acc = 0;
    int frames = 0;

    for (;;) {
        joypad_poll();
        joypad_buttons_t pr = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (pr.a) bilinear = !bilinear;
        if (pr.b) method = (method + 1) % 3;
        if (pr.z) stress_idx = (stress_idx + 1) % 5;
#ifdef T3D_AUTO
        { static int step; static uint32_t ts; if (get_ticks() - ts > 3 * TICKS_PER_SECOND) { ts = get_ticks(); step++; method = (step / 3) % 3; stress_idx = 1 + step % 3; if (step % 3 == 0 && 0) bilinear = !bilinear; } }
#endif
        int n = stress[stress_idx];

        surface_t* disp = display_get();
        rdpq_attach(disp, &zb);
        rdpq_clear(RGBA32(30, 30, 60, 255));
        rdpq_clear_z(0xFFFC);
        uint32_t t0 = get_ticks();
        if (method == 0) setup_mode(bilinear);
        if (n > 0) {
            if (method == 0) draw_rdpq(n);
            else {
                t3d_frame_start();
                t3d_viewport_attach(&vp);
                setup_mode(bilinear);   // after t3d_frame_start(), which sets its own render mode
                t3d_state_set_drawflags(T3D_FLAG_TEXTURED | T3D_FLAG_SHADED | T3D_FLAG_DEPTH);
                t3d_light_set_count(0);
                t3d_matrix_push(mfp);
                draw_quad();
                if (method == 1) draw_t3d_unshared(n); else draw_t3d_shared(n);
                t3d_matrix_pop(1);
            }
        }
        cpu_acc += get_ticks() - t0;
        rdpq_set_mode_standard();
        rdpq_text_printf(&(rdpq_textparms_t){ .style_id = 0 }, 1, 8, 20, "m%d tris %d %s  %.1f ms (cpu %.1f)  %.1f fps",
                         method, n, bilinear ? "bil" : "pt", ms, cpu_ms, fps);
        for (int q = 0; q < probe_n; q++)
            rdpq_text_printf(&(rdpq_textparms_t){ .style_id = 0 }, 1, 8, 40 + q * 10, "%s", probe_txt[q]);
        rdpq_detach_show();

        frames++;
        uint32_t now = get_ticks();
        if (now - t_last > TICKS_PER_SECOND) {
            ms = 1000.0f * (now - t_last) / TICKS_PER_SECOND / frames;
            cpu_ms = 1000.0f * cpu_acc / TICKS_PER_SECOND / frames;
            fps = 1000.0f / ms;
            fprintf(stderr, "method %d tris %d %s: %.2f ms/frame, cpu submit %.2f ms, %.1f fps\n", method, n, bilinear ? "bil" : "pt", ms, cpu_ms, fps);
            t_last = now; cpu_acc = 0; frames = 0;
        }
    }
}

#ifdef T3D_AUTO
// emulator-only (cen64 has no MI repeat mode); linked with -Wl,--wrap=sys_hw_memset{,16,32,64}
void* __wrap_sys_hw_memset(void* p, uint8_t v, size_t n) { memset(p, v, n); data_cache_hit_writeback_invalidate(p, n); return p; }
void* __wrap_sys_hw_memset16(void* p, uint16_t v, size_t n) { uint16_t* q = p; for (size_t i = 0; i < n / 2; i++) q[i] = v; data_cache_hit_writeback_invalidate(p, n); return p; }
void* __wrap_sys_hw_memset32(void* p, uint32_t v, size_t n) { uint32_t* q = p; for (size_t i = 0; i < n / 4; i++) q[i] = v; data_cache_hit_writeback_invalidate(p, n); return p; }
void* __wrap_sys_hw_memset64(void* p, uint64_t v, size_t n) { uint64_t* q = p; for (size_t i = 0; i < n / 8; i++) q[i] = v; data_cache_hit_writeback_invalidate(p, n); return p; }
#endif
