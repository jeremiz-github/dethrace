// OS layer for libdragon / Nintendo 64 (SummerCart64 and other SD-capable flashcarts).
//
// The Carmageddon game data is read from the cartridge's SD card, mounted as "sd:/".
// The game expects a "current directory" it can use relative paths against, but
// libdragon has no chdir()/getcwd(). So we keep our own cwd string and resolve every
// relative path through it. newlib's fopen()/remove() end up calling open()/unlink(),
// and the game calls stat()/mkdir() directly: all four are intercepted at link time
// with `-Wl,--wrap=open,stat,unlink,mkdir` (see cmake/n64-libdragon-toolchain.cmake).
#include "harness/os.h"

#include <libdragon.h>
#include <libcart/cart.h>

// libdragon's debug.h turns debug_init_*() into a literal `false` when NDEBUG is defined (our Release
// build), so the SD card would never be mounted. The real functions are always in libdragon.a: use them.
#undef debug_init_sdfs
#undef debug_init_usblog
#undef debug_init_emulog
bool debug_init_sdfs(const char* prefix, int npart);
bool debug_init_usblog(void);
bool debug_init_emulog(void);
#include <fatfs/ff.h>
#include <fatfs/diskio.h>

#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
extern void dr_dprintf(char* fmt_string, ...);
#include <sys/stat.h>

#define N64_PATH_MAX 256

// Where the game's DATA folder may live on the SD card, in order of preference.
static const char* const kDataRoots[] = {
    "sd:/carmageddon",
    "sd:/carma",
    "sd:/dethrace",
    "sd:/",
};

static int gN64_boot_consoletype;
static int gN64_warm_tries, gN64_warm_rc;
static char gN64_cwd[N64_PATH_MAX] = "rom:/";

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

// Builds an absolute libdragon path ("prefix:/dir/file") from `in`, relative to the cwd.
static void n64_resolve(const char* in, char* out, size_t out_size) {
    const char* slash;
    const char* colon;
    size_t n;

    if (in == NULL) {
        out[0] = '\0';
        return;
    }
    while (in[0] == '.' && in[1] == '/') {
        in += 2;
    }

    colon = strchr(in, ':');
    slash = strchr(in, '/');
    if (colon != NULL && (slash == NULL || colon < slash)) {
        // already has a filesystem prefix ("sd:/...", "rom:/...")
        snprintf(out, out_size, "%s", in);
    } else if (in[0] == '/') {
        // absolute on the current drive
        colon = strchr(gN64_cwd, ':');
        n = colon != NULL ? (size_t)(colon - gN64_cwd + 1) : 0;
        snprintf(out, out_size, "%.*s%s", (int)n, gN64_cwd, in);
    } else {
        n = strlen(gN64_cwd);
        snprintf(out, out_size, "%s%s%s", gN64_cwd, (n > 0 && gN64_cwd[n - 1] == '/') ? "" : "/", in);
    }

    for (n = 0; out[n] != '\0'; n++) {
        if (out[n] == '\\') {
            out[n] = '/';
        }
    }
}

// Linker wrappers (see --wrap in the toolchain file). The __real_* symbols are libdragon's own.
extern int __real_open(const char* path, int flags, ...);
extern int __real_stat(const char* path, struct stat* st);
extern int __real_unlink(char* path);
extern int __real_mkdir(const char* path, mode_t mode);

extern int __real_close(int fd);

// Track open files: libdragon's FAT layer only allows 4 at once, so a leak shows up as ENOENT/EMFILE-like failures.
#define N64_OPEN_SLOTS 16
static struct {
    int fd;
    char path[48];
} gN64_open_files[N64_OPEN_SLOTS];

void OS_N64_DumpOpenFiles(const char* why) {
    int i;
    dr_dprintf("open files (%s):", why);
    for (i = 0; i < N64_OPEN_SLOTS; i++) {
        if (gN64_open_files[i].path[0] != '\0') {
            dr_dprintf("  fd %d %s", gN64_open_files[i].fd, gN64_open_files[i].path);
        }
    }
}

int __wrap_open(const char* path, int flags, ...) {
    char full[N64_PATH_MAX];
    int mode, fd, i;
    va_list ap;

    va_start(ap, flags);
    mode = va_arg(ap, int); // libdragon ignores it; only read to forward it
    va_end(ap);

    n64_resolve(path, full, sizeof(full));
    fd = __real_open(full, flags, mode);
    if (fd >= 0) {
        for (i = 0; i < N64_OPEN_SLOTS; i++) {
            if (gN64_open_files[i].path[0] == '\0') {
                gN64_open_files[i].fd = fd;
                strncpy(gN64_open_files[i].path, full, sizeof(gN64_open_files[i].path) - 1);
                break;
            }
        }
    } else if (strstr(full, "DIAGNOST") == NULL) {
        // a missing optional file is normal; only report when the FAT layer's 4 slots may be exhausted
        int used = 0;
        for (i = 0; i < N64_OPEN_SLOTS; i++) {
            used += gN64_open_files[i].path[0] != '\0';
        }
        if (used >= 4) {
            dr_dprintf("open FAILED errno %d with %d files open: %s", errno, used, full);
            OS_N64_DumpOpenFiles("after failed open");
        }
    }
    return fd;
}

int __wrap_close(int fd) {
    int i;
    for (i = 0; i < N64_OPEN_SLOTS; i++) {
        if (gN64_open_files[i].path[0] != '\0' && gN64_open_files[i].fd == fd) {
            gN64_open_files[i].path[0] = '\0';
            break;
        }
    }
    return __real_close(fd);
}

int __wrap_stat(const char* path, struct stat* st) {
    char full[N64_PATH_MAX];

    n64_resolve(path, full, sizeof(full));
    return __real_stat(full, st);
}

int __wrap_unlink(char* path) {
    char full[N64_PATH_MAX];

    n64_resolve(path, full, sizeof(full));
    return __real_unlink(full);
}

int __wrap_mkdir(const char* path, mode_t mode) {
    char full[N64_PATH_MAX];

    n64_resolve(path, full, sizeof(full));
    return __real_mkdir(full, mode);
}

char* getcwd(char* buf, size_t size) {
    if (buf == NULL || size <= strlen(gN64_cwd)) {
        return NULL;
    }
    strcpy(buf, gN64_cwd);
    return buf;
}

int chdir(const char* path) {
    char full[N64_PATH_MAX];
    size_t n;

    n64_resolve(path, full, sizeof(full));
    n = strlen(full);
    // keep "sd:/" as is, strip any other trailing slash
    while (n > 1 && full[n - 1] == '/' && full[n - 2] != ':') {
        full[--n] = '\0';
    }
    snprintf(gN64_cwd, sizeof(gN64_cwd), "%s", full);
    return 0;
}

int access(const char* path, int mode) {
    struct stat st;

    (void)mode;
    return stat(path, &st) == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Startup helpers (called from main() in src/DETHRACE/main.c)
// ---------------------------------------------------------------------------

// Shows a message on a text console and never returns.
static void n64_fatal_screen(const char* title, const char* message) {
    console_init();
    console_set_debug(false);
    printf("\n  %s\n\n%s\n", title, message);
    console_render();
    for (;;) {
    }
}

static int n64_find_data_root(const char* const* roots, size_t count) {
    size_t i;
    char probe[N64_PATH_MAX];
    struct stat st;

    for (i = 0; i < count; i++) {
        snprintf(probe, sizeof(probe), "%s%sDATA/GENERAL.TXT", roots[i],
            roots[i][strlen(roots[i]) - 1] == '/' ? "" : "/");
        if (stat(probe, &st) == 0) {
            snprintf(gN64_cwd, sizeof(gN64_cwd), "%s", roots[i]);
            return 1;
        }
    }
    return 0;
}

// libdragon's SD driver calls cart_card_init() when mounting. On the user's SummerCart64 the first
// calls fail (FR_NOT_READY) while a cart_init() + cart_card_init() sequence succeeds, so retry that way.
extern int __real_cart_card_init(void);
int gN64_card_init_attempts;

// Every cart_init() call made from libdragon (and from us) goes through here so the diagnostics can show them.
extern int __real_cart_init(void);
static int gN64_cart_init_calls, gN64_cart_init_last_rc = 12345, gN64_calls_before_mount, gN64_calls_after_mount;

int __wrap_cart_init(void) {
    int rc = __real_cart_init();

    gN64_cart_init_calls++;
    gN64_cart_init_last_rc = rc;
    return rc;
}

int __wrap_cart_card_init(void) {
    int rc = -1;

    for (gN64_card_init_attempts = 1; gN64_card_init_attempts <= 6; gN64_card_init_attempts++) {
        rc = __real_cart_card_init();
        if (rc == 0) {
            return 0;
        }
        wait_ms(100);
        cart_init(); // re-establish cartridge access (SC64: unlock registers) then try again
    }
    return rc;
}

// Raw probe of the cartridge's SD interface, to tell why the FAT mount failed.
static void n64_sd_diagnostics(void) {
    static uint8_t sector[512] __attribute__((aligned(16)));
    static const char* const kCartNames[] = { "64drive", "EverDrive X", "EverDrive V1-3", "SummerCart64" };
    char msg[512];
    int n = 0, rc_init, rc_card, rc_read = -99;

    rc_init = cart_init();
    rc_card = cart_card_init();
    if (rc_card == 0) {
        memset(sector, 0, sizeof(sector));
        rc_read = cart_card_rd_dram(sector, 0, 1);
    }
    n += snprintf(msg + n, sizeof(msg) - n, "  boot console type was: %d\n", gN64_boot_consoletype);
    n += snprintf(msg + n, sizeof(msg) - n, "  cart_init calls: %d last rc: %d type: %d\n  calls around 1st mount: %d -> %d\n", gN64_cart_init_calls, gN64_cart_init_last_rc, cart_type, gN64_calls_before_mount, gN64_calls_after_mount);
    n += snprintf(msg + n, sizeof(msg) - n, "  bbplayer now: %d\n", (int)sys_bbplayer());
    n += snprintf(msg + n, sizeof(msg) - n, "  warm-up tries: %d rc: %d\n", gN64_warm_tries, gN64_warm_rc);
    n += snprintf(msg + n, sizeof(msg) - n, "  cart_init: %d  cart: %s\n", rc_init,
        (cart_type >= 0 && cart_type <= 3) ? kCartNames[cart_type] : "none detected");
    n += snprintf(msg + n, sizeof(msg) - n, "  card_init: %d  read sector 0: %d\n", rc_card, rc_read);
    if (rc_read == 0) {
        n += snprintf(msg + n, sizeof(msg) - n, "  boot sig: %02X%02X  jump: %02X  fs: %.5s %.8s\n",
            sector[511], sector[510], sector[0], (char*)sector + 3, (char*)sector + 82);
        n += snprintf(msg + n, sizeof(msg) - n, "  part1 type: %02X  start: %lu\n", sector[450],
            (unsigned long)(sector[454] | (sector[455] << 8) | (sector[456] << 16) | ((unsigned long)sector[457] << 24)));
    }
    if (rc_read == 0) {
        static FATFS probe_fs;
        uint32_t start = sector[454] | (sector[455] << 8) | (sector[456] << 16) | ((uint32_t)sector[457] << 24);
        int rc_vbr = cart_card_rd_dram(sector, start, 1);
        int di = disk_initialize(0);
        int ds = disk_status(0);
        int dr = disk_read(0, sector, start, 1);
        FRESULT fr = f_mount(&probe_fs, "", 1);
        n += snprintf(msg + n, sizeof(msg) - n, "  disk_init: %d status: %d read: %d\n", di, ds, dr);
        n += snprintf(msg + n, sizeof(msg) - n, "  card init attempts: %d\n", gN64_card_init_attempts);
        n += snprintf(msg + n, sizeof(msg) - n, "  vbr read: %d sig: %02X%02X fs: %.8s\n  f_mount: %d\n",
            rc_vbr, sector[511], sector[510], (char*)sector + 82, (int)fr);
    }
    n64_fatal_screen("SD card not mounted", msg);
}

// libdragon copies the "console type" byte left by IPL3 into __boot_consoletype and refuses to use the
// cartridge (USB log, SD card) when it is non-zero (iQue player). Seen non-zero on a real N64 + SummerCart64,
// so decide from the hardware instead (iQue: MI version 0xB_). Must run before any debug_init_*().
void OS_N64_InitDebugLogs(void) {
    debug_init_emulog();
    debug_init_usblog();
}

// The VR4300 raises an "unimplemented operation" exception when a float -> integer conversion overflows (or
// the value is NaN), where the PC just returns a value. The game does this routinely (e.g. srand(position *
// 16777216)), so emulate the conversion with saturation instead of dying, then resume after the instruction.
static exception_handler_t gN64_prev_exception_handler;
volatile int gN64_fpu_fixups;

static void n64_fpu_exception(exception_t* ex) {
    reg_block_t* r = ex->regs;

    if (ex->code == EXCEPTION_CODE_FLOATING_POINT && !(r->cr & 0x80000000u)) { // not in a branch delay slot
        uint32_t insn = *(volatile uint32_t*)(uintptr_t)r->epc;
        uint32_t fmt = (insn >> 21) & 0x1F;
        uint32_t funct = insn & 0x3F;
        uint32_t fs = (insn >> 11) & 0x1F;
        uint32_t fd = (insn >> 6) & 0x1F;
        int is_w = funct == 0x0C || funct == 0x0D || funct == 0x0E || funct == 0x0F || funct == 0x24;
        int is_l = funct == 0x08 || funct == 0x09 || funct == 0x0A || funct == 0x0B || funct == 0x25;

        if ((insn >> 26) == 0x11 && (fmt == 16 || fmt == 17) && (is_w || is_l)) {
            double d;
            if (fmt == 16) {
                uint32_t bits = (uint32_t)r->fpr[fs];
                float f;
                memcpy(&f, &bits, sizeof(f));
                d = f;
            } else {
                memcpy(&d, &r->fpr[fs], sizeof(d));
            }

            if (d != d) {
                d = 0.0;
            } else {
                switch (funct & 3) { // 0/4: round (cvt: current mode = nearest), 1: trunc, 2: ceil, 3: floor
                    case 1:
                        d = d < 0 ? ceil(d) : floor(d);
                        break;
                    case 2:
                        d = ceil(d);
                        break;
                    case 3:
                        d = floor(d);
                        break;
                    default:
                        d = floor(d + 0.5);
                        break;
                }
            }
            if (is_w) {
                int32_t v = d >= 2147483647.0 ? INT32_MAX : (d <= -2147483648.0 ? INT32_MIN : (int32_t)d);
                r->fpr[fd] = (uint32_t)v;
            } else {
                int64_t v = d >= 9.2233720368547758e18 ? INT64_MAX : (d <= -9.2233720368547758e18 ? INT64_MIN : (int64_t)d);
                r->fpr[fd] = (uint64_t)v;
            }
            r->fc31 &= ~C1_CAUSE_MASK;
            r->epc += 4;
            gN64_fpu_fixups++;
            return;
        }
    }
    if (gN64_prev_exception_handler != NULL) {
        gN64_prev_exception_handler(ex);
    }
}

// The PC game relies on IEEE defaults (x/0 = inf, no traps). libdragon turns on FPU traps for overflow,
// divide-by-zero and invalid operations, which kills the game as soon as it divides by zero. Switch the
// traps off (results become inf/NaN like on PC) and flush denormals to zero (the VR4300 cannot handle them).
void OS_N64_ConfigureFPU(void) {
    uint32_t fcr31 = C1_FCR31();

    fcr31 &= ~(C1_ENABLE_MASK | C1_CAUSE_MASK);
    fcr31 |= C1_FCR31_FS;
    C1_WRITE_FCR31(fcr31);
    gN64_prev_exception_handler = register_exception_handler(n64_fpu_exception);
}

void OS_N64_FixConsoleType(void) {
    extern int __boot_consoletype;
    gN64_boot_consoletype = __boot_consoletype;
    if ((*(volatile uint32_t*)0xA4300004 & 0xF0) != 0xB0) {
        __boot_consoletype = 0;
    }
}

// Mounts the SD card (and the ROM's own filesystem), and picks the folder containing the
// game's DATA directory as the current directory. Shows an explanation and stops if there is none.
void OS_N64_InitFilesystem(void) {
    static const char* const kRomRoot[] = { "rom:/" };
    int have_sd;

    dfs_init(DFS_DEFAULT_LOCATION); // optional: only used as a fallback data location
    // debug_init_sdfs() calls cart_init() once and remembers a failure forever. On the SummerCart64 that
    // first call fails (after the USB debug init), while later ones succeed: warm it up ourselves first.
    for (gN64_warm_tries = 1; gN64_warm_tries <= 120; gN64_warm_tries++) {
        gN64_warm_rc = cart_init();
        if (gN64_warm_rc >= 0) {
            break;
        }
        wait_ms(50);
    }
    gN64_calls_before_mount = gN64_cart_init_calls;
    have_sd = debug_init_sdfs("sd:/", -1);
    gN64_calls_after_mount = gN64_cart_init_calls;
    for (int retry = 0; !have_sd && retry < 3; retry++) {
        wait_ms(300); // the cartridge's SD interface may need a moment after boot
        have_sd = debug_init_sdfs("sd:/", -1);
    }

    if (have_sd && n64_find_data_root(kDataRoots, sizeof(kDataRoots) / sizeof(kDataRoots[0]))) {
        return;
    }
    if (n64_find_data_root(kRomRoot, 1)) {
        return;
    }

    if (!have_sd) {
        n64_sd_diagnostics();
    }
    n64_fatal_screen("Carmageddon data not found",
        "  Copy your game's DATA folder to the SD card\n"
        "  so that this file exists:\n\n"
        "    sd:/carmageddon/DATA/GENERAL.TXT\n\n"
        "  (the folder name is case-insensitive.)");
}

// Reads the optional ARGS.TXT (command-line options, one line) from the game directory.
// Returns the number of bytes read (0 if the file is missing or empty); `buffer` is always NUL-terminated.
size_t OS_N64_ReadArgsFile(char* buffer, size_t size) {
    FILE* f;
    size_t n;

    if (size == 0) {
        return 0;
    }
    buffer[0] = '\0';
    f = fopen("ARGS.TXT", "rb");
    if (f == NULL) {
        return 0;
    }
    n = fread(buffer, 1, size - 1, f);
    buffer[n] = '\0';
    fclose(f);
    return n;
}

// ---------------------------------------------------------------------------
// harness/os.h implementation
// ---------------------------------------------------------------------------

void OS_InstallSignalHandler(char* program_name) {
}

void OS_RemoveSignalHandler(void) {
}

static dir_t gDir;
static char gDirPath[N64_PATH_MAX];
static int gDirActive;

// Returns the name of the current entry if it is a regular file, advancing until one is found.
static char* n64_next_regular_file(int advance_first) {
    int r = 0;

    if (advance_first) {
        r = dir_findnext(gDirPath, &gDir);
    }
    while (r == 0) {
        if (gDir.d_type == DT_REG) {
            return gDir.d_name;
        }
        r = dir_findnext(gDirPath, &gDir);
    }
    gDirActive = 0;
    return NULL;
}

char* OS_GetFirstFileInDirectory(char* path) {
    n64_resolve(path, gDirPath, sizeof(gDirPath));
    if (dir_findfirst(gDirPath, &gDir) != 0) {
        gDirActive = 0;
        return NULL;
    }
    gDirActive = 1;
    return n64_next_regular_file(0);
}

char* OS_GetNextFileInDirectory(void) {
    if (!gDirActive) {
        return NULL;
    }
    return n64_next_regular_file(1);
}

FILE* OS_fopen(const char* pathname, const char* mode) {
    // FAT lookups are case-insensitive, so no extra case-folding is needed
    return fopen(pathname, mode);
}

size_t OS_ConsoleReadPassword(char* pBuffer, size_t pBufferLen) {
    return 0;
}

char* OS_Dirname(const char* path) {
    char* copy = strdup(path);
    char* slash;

    if (copy == NULL) {
        return NULL;
    }
    slash = strrchr(copy, '/');
    if (slash == NULL) {
        strcpy(copy, ".");
    } else if (slash == copy) {
        copy[1] = '\0';
    } else {
        *slash = '\0';
    }
    return copy;
}

char* OS_Basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return strdup(slash == NULL ? path : slash + 1);
}

char* OS_GetWorkingDirectory(char* argv0) {
    return strdup(gN64_cwd);
}

int OS_GetPrefPath(char* dest, char* name) {
    // dethrace.ini and saved files live next to DATA, on the SD card
    snprintf(dest, N64_PATH_MAX, "%s%s", gN64_cwd, gN64_cwd[strlen(gN64_cwd) - 1] == '/' ? "" : "/");
    return 0;
}

int OS_GetAdapterAddress(char* name, void* pSockaddr_in) {
    return 0;
}

int OS_InitSockets(void) {
    return 0;
}

void OS_CleanupSockets(void) {
}

int OS_GetLastSocketError(void) {
    return 0;
}

int OS_SetSocketNonBlocking(int socket) {
    return 0;
}

int OS_CloseSocket(int socket) {
    return 1;
}

unsigned int OS_N64_Millis(void) {
    return (unsigned int)(get_ticks_ms());
}

// ---------------------------------------------------------------------------
// Boot options menu: lets the game be tried in different memory/graphics configurations without
// recompiling or editing files. Settings are remembered in OPTIONS.CFG next to the DATA folder.
// ---------------------------------------------------------------------------

int gN64_opponent_limit = -1;  // read by SelectOpponents()
int gN64_texture_quality = 0;
int gN64_physics_steps = 3;    // max physics substeps per frame (read by ApplyPhysicsToCars)  // read by AddPixelmapToStorage()

typedef struct {
    const char* key;
    const char* name;
    int min, max, def;
    const char* const* labels; // optional text for each value (index = value - min)
} tN64_option;

static const char* const kOffOn[] = { "Off", "On" };
static const char* const kTex[] = { "Full", "Half", "Quarter", "Half (256px only)" };
static const char* const kRenderer[] = { "Software", "RDP (hardware)" };
static const char* const kYon[] = { "0.3", "0.4", "0.5", "0.6", "0.7", "0.8", "0.9", "1.0" };

static const tN64_option gN64_option_defs[] = {
    { "opponents", "Opponents", 0, 5, 5, NULL },
    { "textures", "Texture quality", 0, 3, 0, kTex },
    { "sound", "Sound", 0, 1, 0, kOffOn },
    { "cutscenes", "Cutscenes (FMV)", 0, 1, 0, kOffOn },
    { "simple", "Car simplification", 0, 4, 3, NULL },
    { "yon", "Draw distance", 0, 7, 3, kYon },
    { "lomem", "Low-memory mode", 0, 1, 1, kOffOn },
    { "spam", "Spam frittering", 0, 1, 0, kOffOn },
    { "physics", "Physics steps/frm", 1, 5, 3, NULL },
    { "sampler", "CPU sampler (slow)", 0, 1, 1, kOffOn },
    { "diag", "Diagnostics log", 0, 1, 1, kOffOn },
    { "renderer", "Renderer", 0, 1, 1, kRenderer },
};
#define N64_OPTION_COUNT ((int)(sizeof(gN64_option_defs) / sizeof(gN64_option_defs[0])))

static int gN64_option_values[N64_OPTION_COUNT];

static void n64_options_defaults(void) {
    for (int i = 0; i < N64_OPTION_COUNT; i++) {
        gN64_option_values[i] = gN64_option_defs[i].def;
    }
}

static void n64_options_load(void) {
    char line[64];
    FILE* f = fopen("OPTIONS.CFG", "rb");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char* eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        for (int i = 0; i < N64_OPTION_COUNT; i++) {
            if (strcmp(line, gN64_option_defs[i].key) == 0) {
                int v = atoi(eq + 1);
                if (v >= gN64_option_defs[i].min && v <= gN64_option_defs[i].max) {
                    gN64_option_values[i] = v;
                }
            }
        }
    }
    fclose(f);
}

static void n64_options_save(void) {
    FILE* f = fopen("OPTIONS.CFG", "wb");
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < N64_OPTION_COUNT; i++) {
        fprintf(f, "%s=%d\n", gN64_option_defs[i].key, gN64_option_values[i]);
    }
    fclose(f);
}

static int n64_opt(const char* key) {
    for (int i = 0; i < N64_OPTION_COUNT; i++) {
        if (strcmp(key, gN64_option_defs[i].key) == 0) {
            return gN64_option_values[i];
        }
    }
    return 0;
}


// ---- Sampling profiler -------------------------------------------------------------------------
// A timer interrupt records which function was running (via libdragon's backtrace through the
// interrupt frame). OS_N64_SamplerReport() logs the top functions, then resets.
#include <backtrace.h>
extern uint32_t inthandler[];
extern uint32_t inthandler_end[];
extern int OS_N64_InRace(void);

#define N64_SAMPLES_MAX 600
static uint32_t gN64_samples[N64_SAMPLES_MAX];
static volatile int gN64_sample_count;
static timer_link_t* gN64_sampler_timer;

static void n64_sampler_cb(int ovfl) {
    void* buf[8];
    int n, k;
    (void)ovfl;
    if (gN64_sample_count >= N64_SAMPLES_MAX || !OS_N64_InRace()) {
        return;
    }
    n = backtrace(buf, 8);
    for (k = 0; k < n; k++) {
        uint32_t a = (uint32_t)buf[k];
        if (a >= (uint32_t)inthandler && a < (uint32_t)inthandler_end) {
            break;
        }
    }
    if (k + 1 < n) {
        gN64_samples[gN64_sample_count++] = (uint32_t)buf[k + 1];
    }
}

void OS_N64_SamplerStart(void) {
    if (gN64_sampler_timer == NULL && n64_opt("sampler")) {
        timer_init();
        gN64_sampler_timer = new_timer(TIMER_TICKS(40000), TF_CONTINUOUS, n64_sampler_cb);
    }
}

static struct { char name[40]; int count; } gN64_sampler_tab[96];
static int gN64_sampler_tab_n;

static void n64_sampler_frame(void* arg, backtrace_frame_t* f) {
    int i;
    if (f->is_inline) {
        return;
    }
    for (i = 0; i < gN64_sampler_tab_n; i++) {
        if (strcmp(gN64_sampler_tab[i].name, f->func) == 0) {
            gN64_sampler_tab[i].count++;
            return;
        }
    }
    if (gN64_sampler_tab_n < 96) {
        strncpy(gN64_sampler_tab[gN64_sampler_tab_n].name, f->func ? f->func : "?", 39);
        gN64_sampler_tab[gN64_sampler_tab_n].name[39] = '\0';
        gN64_sampler_tab[gN64_sampler_tab_n].count = 1;
        gN64_sampler_tab_n++;
    }
}

void OS_N64_SamplerReport(void (*print)(const char* fmt, ...)) {
    int n = gN64_sample_count, i, j, shown;
    static void* addrs[N64_SAMPLES_MAX];
    if (n < 8) {
        gN64_sample_count = 0;
        return;
    }
    for (i = 0; i < n; i++) {
        addrs[i] = (void*)gN64_samples[i];
    }
    gN64_sample_count = 0;
    gN64_sampler_tab_n = 0;
    for (i = 0; i < n; i++) {
        backtrace_symbols_cb(&addrs[i], 1, 0, n64_sampler_frame, NULL);
    }
    for (shown = 0; shown < 8; shown++) {
        int best = -1;
        for (j = 0; j < gN64_sampler_tab_n; j++) {
            if (gN64_sampler_tab[j].count > 0 && (best < 0 || gN64_sampler_tab[j].count > gN64_sampler_tab[best].count)) {
                best = j;
            }
        }
        if (best < 0) {
            break;
        }
        print("sampler: %2d%% %s (%d/%d)", gN64_sampler_tab[best].count * 100 / n, gN64_sampler_tab[best].name,
            gN64_sampler_tab[best].count, n);
        gN64_sampler_tab[best].count = 0;
    }
}

// Shows the menu, then writes the matching command line into `args` (and sets the runtime options).
void OS_N64_BootMenu(char* args, size_t size) {
    int sel = 0;

    n64_options_defaults();
    n64_options_load();

    console_init();
    console_set_debug(false);
    console_set_render_mode(RENDER_MANUAL);
    joypad_init();

    for (;;) {
        console_clear();
        printf("\n  DETHRACE N64 - debug options\n  ----------------------------\n\n");
        for (int i = 0; i < N64_OPTION_COUNT; i++) {
            const tN64_option* o = &gN64_option_defs[i];
            int v = gN64_option_values[i];
            if (o->labels) {
                printf("  %c %-18s < %s >\n", i == sel ? '>' : ' ', o->name, o->labels[v - o->min]);
            } else {
                printf("  %c %-18s < %d >\n", i == sel ? '>' : ' ', o->name, v);
            }
        }
        printf("\n  D-pad: choose / change\n  A or Start: launch the game\n  Z: reset to defaults\n");
        printf("\n  RAM: %d MB%s\n", (int)(get_memory_size() / (1024 * 1024)), is_memory_expanded() ? "" : " (NO Expansion Pak!)");
        console_render();

        joypad_poll();
        joypad_buttons_t pr = joypad_get_buttons_pressed(JOYPAD_PORT_1);
        if (pr.d_up) {
            sel = (sel + N64_OPTION_COUNT - 1) % N64_OPTION_COUNT;
        } else if (pr.d_down) {
            sel = (sel + 1) % N64_OPTION_COUNT;
        } else if (pr.d_left || pr.d_right) {
            const tN64_option* o = &gN64_option_defs[sel];
            int v = gN64_option_values[sel] + (pr.d_right ? 1 : -1);
            if (v > o->max) {
                v = o->min;
            } else if (v < o->min) {
                v = o->max;
            }
            gN64_option_values[sel] = v;
        } else if (pr.z) {
            n64_options_defaults();
        } else if (pr.a || pr.start) {
            break;
        }
    }

    joypad_close();
    console_close();
    n64_options_save();

    gN64_opponent_limit = n64_opt("opponents");
    gN64_texture_quality = n64_opt("textures");
    {
        extern int gN64_use_rdp;
        gN64_use_rdp = n64_opt("renderer");
    }
    gN64_physics_steps = n64_opt("physics");

    snprintf(args, size, "-noreplay%s%s -simple %d -yon %s%s%s%s",
        n64_opt("lomem") ? " -lomem" : "",
        n64_opt("sound") ? "" : " -nosound",
        n64_opt("simple"),
        kYon[n64_opt("yon")],
        n64_opt("cutscenes") ? "" : " -nocutscenes",
        n64_opt("spam") ? " -spamfritter" : "",
        n64_opt("diag") ? " --enable-diagnostics" : "");
}

// per-phase profiler (see DETHRACE/n64prof.h)
uint32_t gN64_prof_ticks[8];
static uint32_t gN64_prof_start[8];

void OS_N64_ProfBegin(int bucket) {
    gN64_prof_start[bucket] = get_ticks();
}

void OS_N64_ProfEnd(int bucket) {
    gN64_prof_ticks[bucket] += get_ticks() - gN64_prof_start[bucket];
}

// Approximate free heap (bytes) before malloc fails: the arena tops out around 6.2 MB on a 8 MB console.
int OS_N64_HeapHeadroom(void) {
    struct mallinfo mi = mallinfo();
    int ceiling = (int)get_memory_size() - 1850 * 1024 - 100 * 1024;
    return ceiling - mi.uordblks;
}


// Save libdragon assertion messages to ASSERT.TXT (next to DIAGNOST.TXT) before the red error screen appears.
#include <stdarg.h>
extern void __real_debug_assert_func_f(const char* file, int line, const char* func, const char* failedexpr, const char* msg, ...);
void __wrap_debug_assert_func_f(const char* file, int line, const char* func, const char* failedexpr, const char* msg, ...) {
    char text[384];
    char all[512];
    va_list ap;
    FILE* f;
    va_start(ap, msg);
    if (msg != NULL) {
        vsnprintf(text, sizeof(text), msg, ap);
    } else {
        text[0] = 0;
    }
    va_end(ap);
    snprintf(all, sizeof(all), "ASSERT %s:%d (%s) [%s] %s\n", file ? file : "?", line, func ? func : "?", failedexpr ? failedexpr : "", text);
    f = fopen("ASSERT.TXT", "ab");
    if (f != NULL) {
        fputs(all, f);
        fclose(f);
    }
    __real_debug_assert_func_f(file, line, func, failedexpr, "%s", text);
}
