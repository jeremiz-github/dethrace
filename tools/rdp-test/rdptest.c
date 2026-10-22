// Dethrace N64 - RDP feasibility test (milestone 1)
// Textured, perspective-correct, z-buffered, bilinear-filtered triangles at 320x200,
// with the three paletted/RGBA texture formats that fit in TMEM.
//
//   A       : bilinear <-> point sampling
//   B       : texture format (RGBA16 32x32 / CI8 64x32 / CI4 64x64)
//   Z       : stress test (random triangles per frame: 0 / 250 / 500 / 1000 / 2000)
//   D-left/right : rotate camera     D-up/down : move camera forward/back
//   Start   : toggle shading (gouraud darkening with distance)
#include <libdragon.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 200

static surface_t zbuf;
static surface_t tex_rgba16, tex_ci8, tex_ci4;
static uint16_t pal256[256] __attribute__((aligned(8)));
static uint16_t pal16[16] __attribute__((aligned(8)));

// ---------------------------------------------------------------- procedural texture
// A "road + bricks" like pattern so filtering differences are visible.
static void pattern(float u, float v, float* r, float* g, float* b) {
    u -= floorf(u); v -= floorf(v);
    float row = floorf(v * 8.0f);
    float uu = u * 4.0f + ((int)row & 1 ? 0.5f : 0.0f);
    float fu = uu - floorf(uu), fv = v * 8.0f - row;
    float mortar = (fu < 0.07f || fv < 0.10f) ? 1.0f : 0.0f;
    unsigned n = (unsigned)(floorf(u * 64) * 7919 + floorf(v * 64) * 104729);
    n ^= n >> 3; n *= 2654435761u;
    float noise = ((n >> 16) & 255) / 255.0f;
    float base = 0.55f + 0.25f * noise;
    *r = mortar ? 0.75f : base;
    *g = mortar ? 0.72f : base * 0.55f;
    *b = mortar ? 0.68f : base * 0.40f;
}

static uint16_t rgba5551(float r, float g, float b) {
    int R = (int)(r * 31 + 0.5f), G = (int)(g * 31 + 0.5f), B = (int)(b * 31 + 0.5f);
    if (R > 31) R = 31;
    if (G > 31) G = 31;
    if (B > 31) B = 31;
    return (R << 11) | (G << 6) | (B << 1) | 1;
}

// box-filtered sample of the pattern over a texel footprint
static void sample(int x, int y, int w, int h, float* r, float* g, float* b) {
    float sr = 0, sg = 0, sb = 0;
    const int N = 4;
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            float pr, pg, pb;
            pattern((x + (i + 0.5f) / N) / w, (y + (j + 0.5f) / N) / h, &pr, &pg, &pb);
            sr += pr; sg += pg; sb += pb;
        }
    *r = sr / (N * N); *g = sg / (N * N); *b = sb / (N * N);
}

static void make_textures(void) {
    // RGBA16 32x32
    tex_rgba16 = surface_alloc(FMT_RGBA16, 32, 32);
    for (int y = 0; y < 32; y++) {
        uint16_t* row = (uint16_t*)((uint8_t*)tex_rgba16.buffer + y * tex_rgba16.stride);
        for (int x = 0; x < 32; x++) {
            float r, g, b; sample(x, y, 32, 32, &r, &g, &b);
            row[x] = rgba5551(r, g, b);
        }
    }
    // CI8 64x32: RGB332-like palette (3-3-2) -> good enough for the test
    for (int i = 0; i < 256; i++)
        pal256[i] = rgba5551(((i >> 5) & 7) / 7.0f, ((i >> 2) & 7) / 7.0f, (i & 3) / 3.0f);
    tex_ci8 = surface_alloc(FMT_CI8, 64, 32);
    for (int y = 0; y < 32; y++) {
        uint8_t* row = (uint8_t*)tex_ci8.buffer + y * tex_ci8.stride;
        for (int x = 0; x < 64; x++) {
            float r, g, b; sample(x, y, 64, 32, &r, &g, &b);
            int R = (int)(r * 7 + 0.5f), G = (int)(g * 7 + 0.5f), B = (int)(b * 3 + 0.5f);
            row[x] = (R << 5) | (G << 2) | B;
        }
    }
    // CI4 64x64: 16-level brown ramp
    for (int i = 0; i < 16; i++) {
        float t = i / 15.0f;
        pal16[i] = rgba5551(t * 1.05f, t * 0.65f, t * 0.45f);
    }
    tex_ci4 = surface_alloc(FMT_CI4, 64, 64);
    for (int y = 0; y < 64; y++) {
        uint8_t* row = (uint8_t*)tex_ci4.buffer + y * tex_ci4.stride;
        for (int x = 0; x < 64; x += 2) {
            float r, g, b, r2, g2, b2;
            sample(x, y, 64, 64, &r, &g, &b);
            sample(x + 1, y, 64, 64, &r2, &g2, &b2);
            int a = (int)(r * 15 / 1.05f + 0.5f), c = (int)(r2 * 15 / 1.05f + 0.5f);
            if (a > 15) a = 15;
            if (c > 15) c = 15;
            row[x / 2] = (a << 4) | c;
        }
    }
}

// ---------------------------------------------------------------- rendering helpers
typedef struct { float x, y, z, r, g, b, a, s, t, iw; } vtx;  // = TRIFMT_ZBUF_SHADE_TEX

static float cam_x = 0, cam_z = -6, cam_yaw = 0;
static bool shading = true;
static float tex_w = 32, tex_h = 32;

// world (x,y,z) -> screen vertex. Returns false if behind the near plane.
static bool project(vtx* v, float wx, float wy, float wz, float s, float t) {
    float dx = wx - cam_x, dz = wz - cam_z;
    float c = cosf(cam_yaw), sn = sinf(cam_yaw);
    float X = dx * c - dz * sn;
    float Z = dx * sn + dz * c;
    float Y = wy - 1.5f;
    if (Z < 0.3f) return false;
    float iw = 1.0f / Z;
    v->x = W / 2 + X * iw * 190.0f;
    v->y = H / 2 - Y * iw * 190.0f;
    v->z = 1.0f - 0.3f * iw;           // 0 = near, 1 = far (screen-linear)
    float l = shading ? fmaxf(0.25f, 1.25f - Z * 0.07f) : 1.0f;
    v->r = v->g = v->b = l > 1 ? 1 : l;
    v->a = 1;
    v->s = s * tex_w;
    v->t = t * tex_h;
    v->iw = iw;
    return true;
}

static int tri_count;
static void tri(vtx* a, vtx* b, vtx* c) {
    rdpq_triangle(&TRIFMT_ZBUF_SHADE_TEX, (float*)a, (float*)b, (float*)c);
    tri_count++;
}

static void quad(float x0, float y0, float z0, float x1, float y1, float z1,
                 float x2, float y2, float z2, float x3, float y3, float z3, float su, float sv) {
    vtx a, b, c, d;
    if (!project(&a, x0, y0, z0, 0, 0)) return;
    if (!project(&b, x1, y1, z1, su, 0)) return;
    if (!project(&c, x2, y2, z2, su, sv)) return;
    if (!project(&d, x3, y3, z3, 0, sv)) return;
    tri(&a, &b, &c);
    tri(&a, &c, &d);
}

typedef struct { float x[3], y[3], z, s[3], t[3]; } rtri;
#define MAX_RTRI 2000
static rtri rtris[MAX_RTRI];

static void make_random_tris(void) {
    srand(1234);
    for (int i = 0; i < MAX_RTRI; i++) {
        float cx = rand() % W, cy = rand() % H;
        float size = 10 + rand() % 30;   // ~ medium triangles like a 3D scene
        for (int k = 0; k < 3; k++) {
            rtris[i].x[k] = cx + ((rand() % 200) - 100) * size / 100.0f;
            rtris[i].y[k] = cy + ((rand() % 200) - 100) * size / 100.0f;
            rtris[i].s[k] = (rand() % 100) / 100.0f;
            rtris[i].t[k] = (rand() % 100) / 100.0f;
        }
        rtris[i].z = (rand() % 1000) / 1000.0f;
    }
}

static void draw_random(int n) {
    for (int i = 0; i < n; i++) {
        vtx v[3];
        for (int k = 0; k < 3; k++) {
            v[k] = (vtx){ rtris[i].x[k], rtris[i].y[k], rtris[i].z, 0.8f, 0.8f, 0.8f, 1,
                          rtris[i].s[k] * tex_w * 2, rtris[i].t[k] * tex_h * 2, 1.0f };
        }
        tri(&v[0], &v[1], &v[2]);
    }
}

int main(void) {
    debug_init_isviewer();
    debug_init_usblog();
    display_init((resolution_t){ .width = W, .height = H, .interlaced = false },
                 DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();
    rdpq_init();

    zbuf = surface_alloc(FMT_RGBA16, W, H);
    make_textures();
    make_random_tris();
    rdpq_font_t* font = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO);
    rdpq_text_register_font(1, font);

    bool bilinear = true;
    int fmt = 0;
    int stress_idx = 0;
    const int stress[] = { 0, 250, 500, 1000, 2000 };
    float fps = 0;
    unsigned long last_ms = get_ticks_ms();
    int frames = 0;

    for (;;) {
        joypad_poll();
        joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
        if (pressed.a) bilinear = !bilinear;
        if (pressed.b) fmt = (fmt + 1) % 3;
        if (pressed.z) stress_idx = (stress_idx + 1) % 5;
        if (pressed.start) shading = !shading;
        if (held.d_left) cam_yaw -= 0.03f;
        if (held.d_right) cam_yaw += 0.03f;
        if (held.d_up) { cam_x += sinf(cam_yaw) * 0.1f; cam_z += cosf(cam_yaw) * 0.1f; }
        if (held.d_down) { cam_x -= sinf(cam_yaw) * 0.1f; cam_z -= cosf(cam_yaw) * 0.1f; }

        surface_t* disp = display_get();
        rdpq_attach_clear(disp, &zbuf);

        // ---- texture setup for the chosen format
        rdpq_set_mode_standard();
        rdpq_mode_zbuf(true, true);
        rdpq_mode_persp(true);
        rdpq_mode_filter(bilinear ? FILTER_BILINEAR : FILTER_POINT);
        rdpq_mode_combiner(RDPQ_COMBINER_TEX_SHADE);
        rdpq_mode_dithering(DITHER_BAYER_BAYER);
        rdpq_texparms_t tp = { .s.repeats = REPEAT_INFINITE, .t.repeats = REPEAT_INFINITE };
        const char* fname;
        if (fmt == 0) {
            rdpq_mode_tlut(TLUT_NONE);
            rdpq_tex_upload(TILE0, &tex_rgba16, &tp);
            tex_w = 32; tex_h = 32; fname = "RGBA16 32x32";
        } else if (fmt == 1) {
            rdpq_tex_upload_tlut(pal256, 0, 256);
            rdpq_mode_tlut(TLUT_RGBA16);
            rdpq_tex_upload(TILE0, &tex_ci8, &tp);
            tex_w = 64; tex_h = 32; fname = "CI8 64x32 (256 col)";
        } else {
            rdpq_tex_upload_tlut(pal16, 0, 16);
            rdpq_mode_tlut(TLUT_RGBA16);
            tp.palette = 0;
            rdpq_tex_upload(TILE0, &tex_ci4, &tp);
            tex_w = 64; tex_h = 64; fname = "CI4 64x64 (16 col)";
        }

        tri_count = 0;
        // floor: 12x12 quads, texture repeats once per quad
        for (int zi = 0; zi < 12; zi++)
            for (int xi = -6; xi < 6; xi++)
                quad(xi, 0, zi + 1, xi + 1, 0, zi + 1, xi + 1, 0, zi + 2, xi, 0, zi + 2, 1, 1);
        // a wall and a crossing wall to exercise the z-buffer
        quad(-2, 0, 8, 2, 0, 8, 2, 3, 8, -2, 3, 8, 2, 1.5f);
        quad(0, 0, 6, 0, 0, 10, 0, 3, 10, 0, 3, 6, 2, 1.5f);
        draw_random(stress[stress_idx]);

        // overlay text
        rdpq_set_mode_standard();
        rdpq_text_printf(NULL, 1, 6, 14, "%.1f fps  %d tris  %s  %s", fps, tri_count, fname, bilinear ? "BILINEAR" : "POINT");
        rdpq_detach_show();

        frames++;
        unsigned long now = get_ticks_ms();
        if (now - last_ms >= 1000) {
            fps = frames * 1000.0f / (now - last_ms);
            frames = 0;
            last_ms = now;
            debugf("fps %.1f  tris/frame %d  fmt %s  %s\n", fps, tri_count, fname, bilinear ? "bilinear" : "point");
        }
    }
}
