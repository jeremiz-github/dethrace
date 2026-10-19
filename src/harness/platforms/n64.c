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

static uint16_t converted_palette[256];
static br_uint_32 key_state[8];
static void (*gKeyHandler_func)(void) = NULL;
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

static void N64_Harness_ProcessWindowMessages(void) {
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    joypad_buttons_t b = in.btn;

    gPad_stick_x = in.stick_x;
    gPad_stick_y = in.stick_y;
    gPad_buttons = b.d_up | (b.d_down << 1) | (b.d_left << 2) | (b.d_right << 3) | (b.start << 4) | (b.a << 5) | (b.b << 6);

    memset(wanted_keys, 0, sizeof(wanted_keys));

    // Menus: d-pad / stick act as the arrow keys, Start as Return.
    want_key(SCANCODE_UP, b.d_up || in.stick_y > 40);
    want_key(SCANCODE_DOWN, b.d_down || in.stick_y < -40);
    want_key(SCANCODE_LEFT, b.d_left || in.stick_x < -40);
    want_key(SCANCODE_RIGHT, b.d_right || in.stick_x > 40);
    want_key(SCANCODE_RETURN, b.start);

    // L+R together = Escape (pause / menu); alone they are the camera and power-up buttons.
    int both_shoulders = b.l && b.r;
    want_key(SCANCODE_ESCAPE, both_shoulders);

    // In the race: drive through the game's own key map, so it works whatever the KEYMAP file says.
    //   stick = steer, A = accelerate, B = brake/reverse, C-down = handbrake, Z = wheelspin,
    //   C-up = recover, C-right = repair, C-left = horn, R = power-up, L = camera (cockpit view)
    set_action(46, in.stick_x < -20 || b.d_left);        // steer left
    set_action(47, in.stick_x > 20 || b.d_right);        // steer right
    set_action(48, b.a);                                 // accelerate
    set_action(49, b.b);                                 // brake / reverse
    set_action(53, b.c_down);                            // handbrake
    set_action(12, b.z);                                 // hold wheelspin
    set_action(59, b.c_up);                              // KEYMAP_RECOVERY
    set_action(44, b.c_right);                           // KEYMAP_REPAIR
    set_action(58, b.c_left);                            // horn
    set_action(63, b.r && !both_shoulders);              // KEYMAP_BUY_POWER (power-up)
    set_action(45, b.l && !both_shoulders);              // KEYMAP_COCKPIT (camera)

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
static int N64_Harness_ShowErrorMessage(char* title, char* message) {
    struct mallinfo mi = mallinfo();

    fprintf(stderr, "%s: %s\n", title, message);
    if (video_ready) {
        display_close();
        video_ready = 0;
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
        last_report_ms = now;
        frames = 0;
    }
}

static void N64_Harness_Swap(br_pixelmap* back_buffer) {
    int x, y;
    int out_w, out_h;
    int step;
    int off_x, off_y;
    surface_t* disp;

    N64_Harness_ProcessWindowMessages();

    last_screen_src = back_buffer;
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

    // The display buffer is uncached RDRAM: write two RGBA5551 pixels per 32-bit store
    for (y = 0; y < out_h; y++) {
        const uint8_t* src = (const uint8_t*)back_buffer->pixels + (size_t)(y * step) * back_buffer->row_bytes;
        uint32_t* dst = (uint32_t*)((uint8_t*)disp->buffer + (size_t)(y + off_y) * disp->stride) + (off_x >> 1);

        if (step == 1) {
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
