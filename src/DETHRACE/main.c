
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
#include <windows.h>
#endif

#include "brender.h"

extern int Harness_Init(int* argc, char* argv[]);
extern int Harness_Quit(void);
extern int original_main(int pArgc, char* pArgv[]);

void BR_CALLBACK _BrBeginHook(void) {
    struct br_device* BR_EXPORT BrDrv1SoftPrimBegin(char* arguments);
    struct br_device* BR_EXPORT BrDrv1SoftRendBegin(char* arguments);
    struct br_device* BR_EXPORT BrDrv1VirtualFramebufferBegin(char* arguments);
#ifndef DETHRACE_PLATFORM_N64
    struct br_device* BR_EXPORT BrDrv1GLBegin(char* arguments);
#endif

#if _MSC_VER != 1020
    BrDevAddStatic(NULL, BrDrv1SoftPrimBegin, NULL);
    BrDevAddStatic(NULL, BrDrv1SoftRendBegin, NULL);
    BrDevAddStatic(NULL, BrDrv1VirtualFramebufferBegin, NULL);
#ifndef DETHRACE_PLATFORM_N64
    BrDevAddStatic(NULL, BrDrv1GLBegin, NULL);
#endif
#endif
}

void BR_CALLBACK _BrEndHook(void) {
}

#ifdef DETHRACE_PLATFORM_N64
static int dethrace_main(int argc, char* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    int result;

#ifdef _WIN32
#if _MSC_VER != 1020
    /* Attach to the console that started us if any */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        /* We attached successfully, lets redirect IO to the consoles handles if not already redirected */
        if (_fileno(stdout) == -2 || _get_osfhandle(_fileno(stdout)) == -2) {
            freopen("CONOUT$", "w", stdout);
        }

        if (_fileno(stderr) == -2 || _get_osfhandle(_fileno(stderr)) == -2) {
            freopen("CONOUT$", "w", stderr);
        }

        if (_fileno(stdin) == -2 || _get_osfhandle(_fileno(stdin)) == -2) {
            freopen("CONIN$", "r", stdin);
        }
    }
#endif
#endif

    result = Harness_Init(&argc, argv);
    if (result != 0) {
        return result;
    }

    result = original_main(argc, argv);

    Harness_Quit();

    return result;
}

#ifdef DETHRACE_PLATFORM_N64
#include <libdragon.h>
#include <string.h>

extern void OS_N64_InitFilesystem(void);
extern void OS_N64_FixConsoleType(void);
extern void OS_N64_InitDebugLogs(void);
extern size_t OS_N64_ReadArgsFile(char* buffer, size_t size);

// Defaults chosen to fit the N64's 4/8 MB of RAM and its CPU:
//   -noreplay      the action replay buffer is ~14.7 MB otherwise (!)
//   -lomem         the game's own low-memory ("austere") mode
//   -nosound       no audio backend on this port yet, and it saves the sample memory
//   -nocutscenes   skip the Smacker FMVs
//   -simple 3      simplify far-away opponent cars sooner
//   -yon 0.6       shorter draw distance, fewer polygons
// Override by putting your own line of arguments in ARGS.TXT next to the DATA folder.
#define N64_DEFAULT_ARGS "-noreplay -lomem -nosound -nocutscenes -simple 3 -yon 0.6 --enable-diagnostics"

// libdragon entry point: set up logging and the filesystems, then hand over to the game.
int main(void) {
    static char args_buf[512];
    static char* argv[32];
    int argc = 0;
    char* tok;
    size_t n;

    OS_N64_FixConsoleType();
    OS_N64_InitDebugLogs();
    OS_N64_InitFilesystem();

    if (!is_memory_expanded()) {
        debugf("dethrace: WARNING, no Expansion Pak (only 4 MB of RAM): the game will most likely run out of memory\n");
    }

    argv[argc++] = "dethrace";
    n = OS_N64_ReadArgsFile(args_buf, sizeof(args_buf));
    if (n == 0) {
        strcpy(args_buf, N64_DEFAULT_ARGS);
    }
    for (tok = strtok(args_buf, " \t\r\n"); tok != NULL && argc < 31; tok = strtok(NULL, " \t\r\n")) {
        argv[argc++] = tok;
    }
    argv[argc] = NULL;

    return dethrace_main(argc, argv);
}
#endif
