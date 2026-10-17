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
#include <fatfs/ff.h>
#include <fatfs/diskio.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N64_PATH_MAX 256

// Where the game's DATA folder may live on the SD card, in order of preference.
static const char* const kDataRoots[] = {
    "sd:/carmageddon",
    "sd:/carma",
    "sd:/dethrace",
    "sd:/",
};

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

int __wrap_open(const char* path, int flags, ...) {
    char full[N64_PATH_MAX];
    int mode;
    va_list ap;

    va_start(ap, flags);
    mode = va_arg(ap, int); // libdragon ignores it; only read to forward it
    va_end(ap);

    n64_resolve(path, full, sizeof(full));
    return __real_open(full, flags, mode);
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

// Mounts the SD card (and the ROM's own filesystem), and picks the folder containing the
// game's DATA directory as the current directory. Shows an explanation and stops if there is none.
void OS_N64_InitFilesystem(void) {
    static const char* const kRomRoot[] = { "rom:/" };
    int have_sd;

    dfs_init(DFS_DEFAULT_LOCATION); // optional: only used as a fallback data location
    have_sd = debug_init_sdfs("sd:/", -1);
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
