# CMake toolchain file: Nintendo 64 via libdragon (mips64-elf-gcc).
#
#   export N64_INST=/opt/libdragon
#   cmake -S . -B build-n64 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/n64-libdragon-toolchain.cmake \
#       -DDETHRACE_PLATFORM_N64=ON ...
#
if(NOT DEFINED ENV{N64_INST})
    set(N64_INST "/opt/libdragon")
else()
    set(N64_INST "$ENV{N64_INST}")
endif()

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR mips64)
set(N64 TRUE)

set(N64_TARGET mips64-elf)
set(N64_BINDIR "${N64_INST}/bin")
set(N64_INCLUDEDIR "${N64_INST}/${N64_TARGET}/include")
set(N64_LIBDIR "${N64_INST}/${N64_TARGET}/lib")

set(CMAKE_C_COMPILER   "${N64_BINDIR}/${N64_TARGET}-gcc")
set(CMAKE_CXX_COMPILER "${N64_BINDIR}/${N64_TARGET}-g++")
set(CMAKE_ASM_COMPILER "${N64_BINDIR}/${N64_TARGET}-gcc")
set(CMAKE_AR           "${N64_BINDIR}/${N64_TARGET}-gcc-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${N64_BINDIR}/${N64_TARGET}-gcc-ranlib" CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${N64_BINDIR}/${N64_TARGET}-objcopy" CACHE FILEPATH "")

# Bare-metal target: never try to link a test executable
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Same ABI/CPU flags as libdragon's n64.mk (o64 ABI: 32-bit int/long/pointers)
# newlib's int32_t is `long` while this codebase mixes `int`/`long` for 32-bit types
# (identical size/representation on the o64 ABI): accept that, and disable strict aliasing.
set(N64_COMMON_FLAGS "-march=vr4300 -mtune=vr4300 -mabi=o64 -I${N64_INCLUDEDIR} -ffunction-sections -fdata-sections -DN64 -fno-strict-aliasing -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types")
set(CMAKE_C_FLAGS_INIT "${N64_COMMON_FLAGS} -std=gnu17")
set(CMAKE_CXX_FLAGS_INIT "${N64_COMMON_FLAGS} -std=gnu++17")
set(CMAKE_ASM_FLAGS_INIT "${N64_COMMON_FLAGS}")

set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG_INIT "-O1 -g")

# Linking: libdragon's linker script + libs, as in n64.mk
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${N64_LIBDIR} -Wl,-T${N64_LIBDIR}/n64.ld -Wl,--gc-sections -Wl,--wrap=__do_global_ctors -Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=stat -Wl,--wrap=unlink -Wl,--wrap=mkdir -Wl,--wrap=cart_card_init -Wl,--wrap=cart_init")
set(CMAKE_C_STANDARD_LIBRARIES "-ldragon -lm -ldragonsys -lc")

set(CMAKE_FIND_ROOT_PATH "${N64_INST}/${N64_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# No dlopen / pthread on bare metal
set(CMAKE_DL_LIBS "")
