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

static void N64_Harness_ProcessWindowMessages(void) {
    joypad_poll();
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    joypad_buttons_t b = in.btn;

    set_key(SCANCODE_UP, b.d_up || in.stick_y > 40);
    set_key(SCANCODE_DOWN, b.d_down || in.stick_y < -40);
    set_key(SCANCODE_LEFT, b.d_left || in.stick_x < -40);
    set_key(SCANCODE_RIGHT, b.d_right || in.stick_x > 40);
    set_key(SCANCODE_RETURN, b.start);
    set_key(SCANCODE_SPACE, b.a);                        // handbrake / select
    set_key(SCANCODE_LCONTROL, b.b);                     // fire / back
    set_key(SCANCODE_ESCAPE, b.l && b.r);                // pause / menu
    set_key(SCANCODE_TAB, b.z);                          // switch view
    set_key(SCANCODE_LSHIFT, b.c_up);                    // turbo
    set_key(SCANCODE_F1, b.c_left);
    set_key(SCANCODE_F2, b.c_right);
    set_key(SCANCODE_F3, b.c_down);
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

static int N64_Harness_ShowErrorMessage(char* title, char* message) {
    fprintf(stderr, "%s: %s\n", title, message);
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
        debugf("dethrace: %.1f fps, heap in use %d KB of %d KB RAM\n",
            frames * 1000.0f / (float)(now - last_report_ms),
            (int)(mi.uordblks / 1024), (int)(get_memory_size() / 1024));
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
