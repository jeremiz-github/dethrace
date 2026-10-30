// Nintendo 64 platform driver (libdragon).
//
// Video: the game renders into an 8-bit paletted br_pixelmap. We expand it to
// RGBA5551 into libdragon's display surface on every Swap().
// Input: the N64 pad is mapped onto the PC (DirectInput) scancodes the game
// expects.
#include "dethrace_scancodes.h"
#include "harness.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/trace.h"

#include <libdragon.h>

extern void dr_dprintf(char* fmt_string, ...);
extern volatile int gN64_fpu_fixups;

#include <malloc.h>
#include <stdio.h>
#include <string.h>

extern void QuitGame(void);

#define N64_SCREEN_W 320
#define N64_SCREEN_H 240

extern volatile int gN64_stage;
extern volatile uint32_t gN64_progress;
static uint16_t converted_palette[256];
static br_uint_32 key_state[8];
static void (*gKeyHandler_func)(void) = NULL;

extern int gN64_use_rdp;
extern void* gN64_screen_base;
extern int gN64_screen_w, gN64_screen_h, gN64_screen_stride;
extern void N64_Rdp_PaletteChanged(const uint16_t* pal5551);
extern int N64_Rdp_FrameActive(void);
extern void N64_Rdp_FrameDone(void);
extern const uint16_t* N64_Rdp_Row(int y);
static br_pixelmap* last_screen_src = NULL;
static int video_ready = 0;

static void set_key(int scancode, int down) {
    int word = scancode >> 5;
    br_uint_32 mask = 1u << (scancode & 0x1F);
    int was_down = (key_state[word] & mask) != 0;

    if (down == was_down) {
        return;
    }
    if (down) {
        key_state[word] |= mask;
    } else {
        key_state[word] &= ~mask;
    }
    if (gKeyHandler_func != NULL) {
        gKeyHandler_func();
    }
}

// keypad 4 / 6 (the game's KEY_KP_4 / KEY_KP_6 key codes): free keys used for steering with the stick
#define N64_KEY_STEER_LEFT 85
#define N64_KEY_STEER_RIGHT 87
extern int gKey_mapping[67];       // the game's action -> key table (from KEYMAP_n.TXT)
extern unsigned char gScan_code[123][2];

// Keys wanted down this frame (several pad inputs can map onto the same key): applied once at the end.
static br_uint_32 wanted_keys[8];

static void want_key(int scancode, int down) {
    if (down) {
        wanted_keys[scancode >> 5] |= 1u << (scancode & 0x1F);
    }
}

// Presses the key the game currently has bound to action `action` (an index into gKey_mapping).
static void set_action(int action, int down) {
    int key = gKey_mapping[action];

    if (key >= 0 && key < 115 && gScan_code[key][0] != 0) { // 115+ are joystick axes/buttons, not keys
        want_key(gScan_code[key][0], down);
    }
}

static int gPad_stick_x, gPad_stick_y;
static unsigned gPad_buttons;

// gProgram_state.racing (controls.c helper): true only while the car is actually being driven, not in the pause menu
// or any other screen. The buttons drive the car then, and navigate the menus otherwise.
extern int OS_N64_InRace(void);
extern int gJoy_array[8];

static void N64_Harness_ProcessWindowMessages(void) {
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    joypad_buttons_t b = in.btn;

    gPad_stick_x = in.stick_x;
    gPad_stick_y = in.stick_y;
    gPad_buttons = b.d_up | (b.d_down << 1) | (b.d_left << 2) | (b.d_right << 3) | (b.start << 4) | (b.a << 5) | (b.b << 6);

    int in_race = OS_N64_InRace();

    // The stick must only steer the wheels, not turn the camera: the camera turns with the arrow keys, which
    // is also what the default key map uses for steering. Steering is rebound to the game's joystick axes 0 and 1
    // (key codes 115/116), which give analog steering (0..65535 each way) and never touch the keyboard.
    gKey_mapping[46] = 115;
    gKey_mapping[47] = 116;
    gJoy_array[0] = -1;
    gJoy_array[1] = -1;

    memset(wanted_keys, 0, sizeof(wanted_keys));

    // d-pad: arrow keys (menu navigation; in a race they turn the camera)
    want_key(SCANCODE_UP, b.d_up);
    want_key(SCANCODE_DOWN, b.d_down);
    want_key(SCANCODE_LEFT, b.d_left);
    want_key(SCANCODE_RIGHT, b.d_right);

    if (in_race) {
        {
            int x = in.stick_x;
            int mag = x < 0 ? -x : x;
            int amount = 0;
            if (mag > 8) { // dead zone; the N64 stick reaches about 80
                amount = (mag - 8) * 65535 / 68;
                if (amount > 65535) {
                    amount = 65535;
                }
            }
            if (amount > 0) {
                gJoy_array[x < 0 ? 0 : 1] = amount;
            }
        }
        set_action(48, b.a);                             // accelerate
        set_action(49, b.b);                             // brake / reverse
        want_key(SCANCODE_ESCAPE, b.start);              // pause menu
    } else {
        want_key(SCANCODE_UP, in.stick_y > 40);          // menus: stick = arrows
        want_key(SCANCODE_DOWN, in.stick_y < -40);
        want_key(SCANCODE_LEFT, in.stick_x < -40);
        want_key(SCANCODE_RIGHT, in.stick_x > 40);
        want_key(SCANCODE_RETURN, b.a || b.start);       // validate
        want_key(SCANCODE_ESCAPE, b.b);                  // back
    }
    set_action(53, b.c_down);                            // handbrake
    set_action(12, b.z);                                 // hold wheelspin
    set_action(59, b.c_up);                              // KEYMAP_RECOVERY
    set_action(44, b.c_right);                           // KEYMAP_REPAIR
    set_action(58, b.c_left);                            // horn
    set_action(45, b.r);                                 // KEYMAP_COCKPIT: cockpit / exterior view
    set_action(56, b.l);                                 // KEYMAP_MAP

    // apply: report every key whose state changed
    for (int sc = 0; sc < 256; sc++) {
        set_key(sc, (wanted_keys[sc >> 5] >> (sc & 0x1F)) & 1);
    }
}

static void N64_Harness_SetKeyHandler(void (*handler_func)(void)) {
    gKeyHandler_func = handler_func;
}

static void N64_Harness_GetKeyboardState(br_uint_32* buffer) {
    memcpy(buffer, key_state, sizeof(key_state));
}

static int N64_Harness_GetMouseButtons(int* pButton1, int* pButton2) {
    *pButton1 = 0;
    *pButton2 = 0;
    return 0;
}

static int N64_Harness_GetMousePosition(int* pX, int* pY) {
    return 0;
}

static int N64_Harness_SetWindowPos(void* hWnd, int x, int y, int nWidth, int nHeight) {
    return 0;
}

static int N64_Harness_ShowCursor(int show) {
    return 0;
}

static void N64_Harness_Sleep(br_uint_32 dwMilliseconds) {
    wait_ms(dwMilliseconds);
}

static br_uint_32 N64_Harness_GetTicks(void) {
    return (br_uint_32)get_ticks_ms();
}

// Fatal errors: show the message on a text screen (also in the DIAGNOST.TXT log) and stop.
// Largest block malloc can still hand out (binary search): shows heap fragmentation.
static size_t n64_largest_free_block(void) {
    size_t lo = 0, hi = 6u * 1024 * 1024;
    while (hi - lo > 4096) {
        size_t mid = (lo + hi) / 2;
        void* p = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void n64_draw_wrapped(surface_t* disp, int x, int* y, const char* text) {
    char line[48];
    int n = 0;
    for (const char* c = text;; c++) {
        if (*c == '\0' || *c == '\n' || n >= 36) {
            line[n] = '\0';
            graphics_draw_text(disp, x, *y, line);
            *y += 10;
            n = 0;
            if (*c == '\0') {
                break;
            }
            if (*c == '\n') {
                continue;
            }
        }
        if (*c != '\n') {
            line[n++] = *c;
        }
    }
}

static int N64_Harness_ShowErrorMessage(char* title, char* message) {
    struct mallinfo mi = mallinfo();
    size_t largest = n64_largest_free_block();

    fprintf(stderr, "%s: %s\n", title, message);
    dr_dprintf("heap at error: in use %d KB, arena %d KB, free %d KB in %d chunks, largest block %d KB",
        (int)(mi.uordblks / 1024), (int)(mi.arena / 1024), (int)(mi.fordblks / 1024), (int)mi.ordblks, (int)(largest / 1024));
    {
        extern void OS_N64_DumpMemoryByClass(void);
        OS_N64_DumpMemoryByClass();
    }
    dr_dprintf("Displaying fatal error...");
    if (video_ready) {
        // draw straight into the current display buffers: allocating a console could fail when memory ran out
        for (int pass = 0; pass < 2; pass++) {
            surface_t* disp = display_get();
            int y = 16;
            graphics_fill_screen(disp, 0);
            graphics_set_color(0xFFFFFFFF, 0);
            n64_draw_wrapped(disp, 12, &y, title);
            y += 10;
            n64_draw_wrapped(disp, 12, &y, message);
            y += 10;
            {
                char info[96];
                snprintf(info, sizeof(info), "heap in use %d KB, largest free %d KB", (int)(mi.uordblks / 1024), (int)(largest / 1024));
                n64_draw_wrapped(disp, 12, &y, info);
            }
            n64_draw_wrapped(disp, 12, &y, "(details in DIAGNOST.TXT on the SD card)");
            display_show(disp);
        }
        for (;;) {
        }
    }
    console_init();
    console_set_debug(false);
    printf("\n  %s\n\n  %s\n\n  heap in use: %d KB of %d KB RAM\n  (details in DIAGNOST.TXT on the SD card)\n",
        title, message, (int)(mi.uordblks / 1024), (int)(get_memory_size() / 1024));
    console_render();
    for (;;) {
    }
    return 0;
}

static void N64_Harness_DestroyWindow(void) {
    if (video_ready) {
        display_close();
        video_ready = 0;
    }
}

static void N64_Harness_CreateWindow(const char* title, int width, int height, tHarness_window_type window_type) {
    if (video_ready) {
        return;
    }
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    joypad_init();
    if (gN64_use_rdp) {
        rdpq_init();
    }
    video_ready = 1;
}

static void N64_Harness_PaletteChanged(br_colour entries[256]) {
    int i;
    for (i = 0; i < 256; i++) {
        converted_palette[i] = (uint16_t)(((BR_RED(entries[i]) >> 3) << 11)
            | ((BR_GRN(entries[i]) >> 3) << 6)
            | ((BR_BLU(entries[i]) >> 3) << 1)
            | 1);
    }
    N64_Rdp_PaletteChanged(converted_palette);
}

// Once per ~2 s, log the frame rate and heap usage (visible through the SC64 USB log / ISViewer).
static void n64_report_stats(void) {
    static unsigned long last_report_ms = 0;
    static unsigned frames = 0;
    unsigned long now = get_ticks_ms();

    frames++;
    if (last_report_ms == 0) {
        last_report_ms = now;
        frames = 0;
    } else if (now - last_report_ms >= 2000) {
        struct mallinfo mi = mallinfo();
        dr_dprintf("n64: %d fps*10, heap in use %d KB of %d KB RAM, fpu fixups %d, pad %02x stick %d,%d, keys %08x %08x",
            (int)(frames * 10000.0f / (float)(now - last_report_ms)),
            (int)(mi.uordblks / 1024), (int)(get_memory_size() / 1024), gN64_fpu_fixups,
            gPad_buttons, gPad_stick_x, gPad_stick_y, (unsigned)key_state[6], (unsigned)key_state[0]);
        {
            // average milliseconds per frame spent in each phase of a race frame (0 outside races)
            extern uint32_t gN64_prof_ticks[24];
            float ms_per_tick = 1000.0f / (float)TICKS_PER_SECOND / (float)(frames ? frames : 1);
            dr_dprintf("prof ms/frame: oppo %d phys %d cargfx %d peds %d | render %d (scene3d %d shadows %d) swap %d",
                (int)(gN64_prof_ticks[0] * ms_per_tick), (int)(gN64_prof_ticks[1] * ms_per_tick),
                (int)(gN64_prof_ticks[2] * ms_per_tick), (int)(gN64_prof_ticks[3] * ms_per_tick),
                (int)(gN64_prof_ticks[4] * ms_per_tick), (int)(gN64_prof_ticks[5] * ms_per_tick),
                (int)(gN64_prof_ticks[6] * ms_per_tick), (int)(gN64_prof_ticks[7] * ms_per_tick));
            dr_dprintf("prof2 ms/frame: rdptri %d (tex %d) rdpend %d | v1model %d renderBegin %d | track %d nontrack %d effects %d",
                (int)(gN64_prof_ticks[8] * ms_per_tick), (int)(gN64_prof_ticks[9] * ms_per_tick),
                (int)(gN64_prof_ticks[10] * ms_per_tick), (int)(gN64_prof_ticks[11] * ms_per_tick),
                (int)(gN64_prof_ticks[12] * ms_per_tick), (int)(gN64_prof_ticks[13] * ms_per_tick),
                (int)(gN64_prof_ticks[14] * ms_per_tick), (int)(gN64_prof_ticks[15] * ms_per_tick));
            dr_dprintf("prof3 ms/frame: rdpq-call %d setmode %d fillvert %d",
                (int)(gN64_prof_ticks[16] * ms_per_tick), (int)(gN64_prof_ticks[17] * ms_per_tick),
                (int)(gN64_prof_ticks[18] * ms_per_tick));
            memset(gN64_prof_ticks, 0, sizeof(uint32_t) * 24);
        }
        {
            // once per race: full memory breakdown by class, to guide memory optimisation
            extern int OS_N64_InRace(void);
            extern void OS_N64_DumpMemoryByClass(void);
            static int dumped = 0;
            if (OS_N64_InRace()) {
                if (!dumped && now > 0) {
                    dumped = 1;
                    dr_dprintf("memory breakdown at race start:");
                    OS_N64_DumpMemoryByClass();
                }
            } else {
                dumped = 0;
            }
        }
        {
            extern void OS_N64_SamplerStart(void);
            extern void OS_N64_SamplerReport(void (*print)(const char* fmt, ...));
            OS_N64_SamplerStart();
            OS_N64_SamplerReport(dr_dprintf);
        }
        last_report_ms = now;
        frames = 0;
    }
}

static void N64_Harness_Swap(br_pixelmap* back_buffer) {
    gN64_stage = 1;
    gN64_progress++;
    int x, y;
    int out_w, out_h;
    int step;
    int off_x, off_y;
    surface_t* disp;
    int use_rdp;

    {
        extern void OS_N64_ProfBegin(int);
        OS_N64_ProfBegin(7);
    }
    N64_Harness_ProcessWindowMessages();

    last_screen_src = back_buffer;
    gN64_screen_base = back_buffer->pixels;
    gN64_screen_w = back_buffer->width;
    gN64_screen_h = back_buffer->height;
    gN64_screen_stride = back_buffer->row_bytes;
    disp = display_get();

    // 320x200 (low-res, the default here) is copied 1:1; 640x480 is decimated 2:1.
    step = back_buffer->width >= 2 * N64_SCREEN_W ? 2 : 1;
    out_w = back_buffer->width / step;
    out_h = back_buffer->height / step;
    if (out_w > N64_SCREEN_W) {
        out_w = N64_SCREEN_W;
    }
    if (out_h > N64_SCREEN_H) {
        out_h = N64_SCREEN_H;
    }
    out_w &= ~1; // two pixels per 32-bit store
    off_x = ((N64_SCREEN_W - out_w) / 2) & ~1;
    off_y = (N64_SCREEN_H - out_h) / 2;

    use_rdp = N64_Rdp_FrameActive() && back_buffer->width == 320 && back_buffer->height == 200;
    if (use_rdp) {
        N64_Rdp_FrameDone();
    } else {
        N64_Rdp_FrameDone();
    }
    // The display buffer is uncached RDRAM: write two RGBA5551 pixels per 32-bit store
    for (y = 0; y < out_h; y++) {
        const uint8_t* src = (const uint8_t*)back_buffer->pixels + (size_t)(y * step) * back_buffer->row_bytes;
        uint32_t* dst = (uint32_t*)((uint8_t*)disp->buffer + (size_t)(y + off_y) * disp->stride) + (off_x >> 1);

        if (step == 1 && use_rdp) {
            const uint32_t* r3d = (const uint32_t*)N64_Rdp_Row(y); // uncached: read two pixels at once
            for (x = 0; x < out_w; x += 2) {
                uint32_t r = *r3d++;
                uint16_t p0 = src[x] ? converted_palette[src[x]] : (uint16_t)(r >> 16);
                uint16_t p1 = src[x + 1] ? converted_palette[src[x + 1]] : (uint16_t)r;
                *dst++ = ((uint32_t)p0 << 16) | p1;
            }
        } else if (step == 1) {
            for (x = 0; x < out_w; x += 2) {
                *dst++ = ((uint32_t)converted_palette[src[x]] << 16) | converted_palette[src[x + 1]];
            }
        } else {
            for (x = 0; x < out_w; x += 2) {
                *dst++ = ((uint32_t)converted_palette[src[2 * x]] << 16) | converted_palette[src[2 * x + 2]];
            }
        }
    }
    display_show(disp);
    gN64_stage = 0;
    {
        extern void OS_N64_ProfEnd(int);
        OS_N64_ProfEnd(7);
    }
    n64_report_stats();
}

static void N64_Harness_GetViewport(int* x, int* y, float* width_multipler, float* height_multiplier) {
    *x = 0;
    *y = 0;
    *width_multipler = 1.0f;
    *height_multiplier = 1.0f;
}

static void* N64_Harness_GL_GetProcAddress(const char* name) {
    return NULL;
}

static int N64_Harness_Platform_Init(tHarness_platform* platform) {
    platform->ProcessWindowMessages = N64_Harness_ProcessWindowMessages;
    platform->Sleep = N64_Harness_Sleep;
    platform->GetTicks = N64_Harness_GetTicks;
    platform->ShowCursor = N64_Harness_ShowCursor;
    platform->SetWindowPos = N64_Harness_SetWindowPos;
    platform->DestroyWindow = N64_Harness_DestroyWindow;
    platform->SetKeyHandler = N64_Harness_SetKeyHandler;
    platform->GetKeyboardState = N64_Harness_GetKeyboardState;
    platform->GetMousePosition = N64_Harness_GetMousePosition;
    platform->GetMouseButtons = N64_Harness_GetMouseButtons;
    platform->ShowErrorMessage = N64_Harness_ShowErrorMessage;

    platform->CreateWindow_ = N64_Harness_CreateWindow;
    platform->Swap = N64_Harness_Swap;
    platform->PaletteChanged = N64_Harness_PaletteChanged;
    platform->GL_GetProcAddress = N64_Harness_GL_GetProcAddress;
    platform->GetViewport = N64_Harness_GetViewport;
    return 0;
}

const tPlatform_bootstrap N64_bootstrap = {
    "n64",
    "Nintendo 64 (libdragon)",
    ePlatform_cap_software,
    N64_Harness_Platform_Init,
};
