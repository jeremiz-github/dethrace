// OS layer for libdragon / Nintendo 64.
// Game data is expected inside the ROM's DFS filesystem, mounted as "rom:/".
#include "harness/os.h"

#include <libdragon.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void OS_InstallSignalHandler(char* program_name) {
}

void OS_RemoveSignalHandler(void) {
}

char* OS_GetFirstFileInDirectory(char* path) {
    return NULL;
}

char* OS_GetNextFileInDirectory(void) {
    return NULL;
}

FILE* OS_fopen(const char* pathname, const char* mode) {
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
    return strdup("rom:/");
}

int OS_GetPrefPath(char* dest, char* name) {
    // No writable pref directory on a bare cartridge
    return 1;
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

// ---------------------------------------------------------------------------
// POSIX bits the game calls directly but libdragon's newlib doesn't provide.
// ---------------------------------------------------------------------------
#include <sys/stat.h>

static char gN64_cwd[256] = "rom:/";

char* getcwd(char* buf, size_t size) {
    if (buf == NULL || size <= strlen(gN64_cwd)) {
        return NULL;
    }
    strcpy(buf, gN64_cwd);
    return buf;
}

int chdir(const char* path) {
    if (path == NULL || strlen(path) >= sizeof(gN64_cwd)) {
        return -1;
    }
    strcpy(gN64_cwd, path);
    return 0;
}

int access(const char* path, int mode) {
    struct stat st;

    (void)mode;
    return stat(path, &st) == 0 ? 0 : -1;
}
