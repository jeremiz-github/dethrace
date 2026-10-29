#ifndef N64PROF_H
#define N64PROF_H

// Tiny per-phase profiler for the N64 port: PROF(bucket, statement) times a statement with the CPU counter.
// The platform layer prints the average milliseconds per frame of every bucket into DIAGNOST.TXT.
enum {
    N64_PROF_OPPONENTS,
    N64_PROF_PHYSICS,
    N64_PROF_CAR_GFX,
    N64_PROF_PEDESTRIANS,
    N64_PROF_RENDER_FRAME,
    N64_PROF_SCENE_3D,
    N64_PROF_SHADOWS,
    N64_PROF_SWAP,
    N64_PROF_RDP_TRI,
    N64_PROF_RDP_TEX,
    N64_PROF_RDP_END,
    N64_PROF_V1MODEL,
    N64_PROF_RENDER_BEGIN,
    N64_PROF_TRACK,
    N64_PROF_NONTRACK,
    N64_PROF_EFFECTS,
    N64_PROF_COUNT
};

#ifdef N64
void OS_N64_ProfBegin(int bucket);
void OS_N64_ProfEnd(int bucket);
#define PROF(bucket, ...)         \
    do {                          \
        OS_N64_ProfBegin(bucket); \
        __VA_ARGS__;              \
        OS_N64_ProfEnd(bucket);   \
    } while (0)
#else
#define PROF(bucket, ...)  \
    do {                   \
        __VA_ARGS__;       \
    } while (0)
#endif

#endif
