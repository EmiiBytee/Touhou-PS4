cmake_minimum_required(VERSION 3.20)

# PS4 build of the reconstructed Touhou 8 engine. This is a full game using GNM directly;
# Piglet only works for mini apps and would disable the console's gameplay recording.
#
# The reconstructed game sources are compiled from the native 64-bit layout branch, not the
# exact 32-bit VC7 target, and reach the platform through the same Direct3D 8, Win32,
# DirectInput and DirectSound boundary the Linux port uses. Only the implementations differ:
# src/modern/ps4/d3d8_gnm.cpp replaces the OpenGL device, and the POSIX/SDL adapter under
# src/modern/linux is shared, selected with TH_PS4.
project(th08_ps4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TH08_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/th08)
set(PS4_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# Kept in the order of TH08_GAME_SOURCES in ${TH08_DIR}/CMakeLists.txt, which is the
# canonical list; the bootstrap target that validated the package layout lives on as
# bootstrap.cpp but is no longer built.
set(TH08_GAME_SOURCES
    AnmManager.cpp AsciiManager.cpp AsciiManagerBossMarker.cpp AsciiManagerGauge.cpp
    AsciiManagerGuiMode.cpp AsciiManagerScale.cpp Background.cpp BulletManager.cpp
    EclDependencies.cpp EclExIns.cpp EclGlobals.cpp EclHelpers.cpp EclManager.cpp
    EclOperandsFloat.cpp EclOperandsInt.cpp EclRun.cpp EffectManager.cpp Ending.cpp
    EnemyManager.cpp EnemyManagerUpdate.cpp EnemyTimeline.cpp GameManager.cpp Global.cpp
    Gui.cpp ItemManager.cpp Midi.cpp MusicRoom.cpp Player.cpp PlayerBomb.cpp
    ReplayManager.cpp ResultScreen.cpp ScoreDat.cpp ScreenEffect.cpp SoundPlayer.cpp
    Spellcard.cpp Supervisor.cpp TextHelper.cpp TitleScreen.cpp main.cpp utils.cpp
    zwave.cpp
    pbg/Lzss.cpp pbg/PbgArchive.cpp pbg/PbgFile.cpp
)
list(TRANSFORM TH08_GAME_SOURCES PREPEND ${TH08_DIR}/src/)

# The platform adapter. d3dx8_compat.cpp and the POSIX halves of linux_compat.cpp and
# linux_runtime.cpp are shared with the Linux port verbatim; render_audit.cpp is a Linux
# diagnostic and is deliberately left out.
set(TH08_PS4_ADAPTER_SOURCES
    ${TH08_DIR}/src/modern/ps4/d3d8_gnm.cpp
    ${TH08_DIR}/src/modern/linux/d3dx8_compat.cpp
    ${TH08_DIR}/src/modern/linux/linux_compat.cpp
    ${TH08_DIR}/src/modern/linux/linux_runtime.cpp
    ${TH08_DIR}/src/modern/ps4/th08_thpatch.cpp
)

# i18n.hpp is generated from config/i18n.csv by the same script the other targets use, so
# the PS4 build cannot drift from them.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(TH08_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
    OUTPUT ${TH08_GENERATED_DIR}/i18n.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TH08_GENERATED_DIR}
    COMMAND ${Python3_EXECUTABLE} ${TH08_DIR}/scripts/generate_i18n.py
            -o ${TH08_GENERATED_DIR}/i18n.hpp --shift-jis ${TH08_DIR}/config/i18n.csv
    DEPENDS ${TH08_DIR}/config/i18n.csv ${TH08_DIR}/scripts/generate_i18n.py
    VERBATIM
)
add_custom_target(th08_generated DEPENDS ${TH08_GENERATED_DIR}/i18n.hpp)

add_executable(th08 ${TH08_GAME_SOURCES} ${TH08_PS4_ADAPTER_SOURCES}
    ${PS4_COMMON_DIR}/ps4_platform.cpp
    ${PS4_COMMON_DIR}/ps4_keyboard.cpp
    ${PS4_COMMON_DIR}/ps4_malloc.cpp
    ${PS4_COMMON_DIR}/thpatch/thpatch.cpp
    ${PS4_COMMON_DIR}/thpatch/json.cpp
)
add_dependencies(th08 th08_generated)

target_include_directories(th08 BEFORE PRIVATE
    # The Win32 and Direct3D 8 declarations are portable; only their implementations are
    # platform specific, so the PS4 target uses the same headers as the Linux one.
    ${TH08_DIR}/src/modern/linux/include
)
target_include_directories(th08 PRIVATE
    ${TH08_DIR}/src ${TH08_GENERATED_DIR} ${PS4_COMMON_DIR} ${PS4_COMMON_DIR}/thpatch)
target_compile_definitions(th08 PRIVATE
    NDEBUG TH_PS4 TH_PS4_BIG_APP TH_PS4_GNM TH_THPATCH
    # IN's MSG scripts: thcrap's "msg08" opcode table and XOR-encrypted text.
    TH_THPATCH_MSG08
    TH08_MODERN_PORT TH08_PORTABLE_NATIVE_LAYOUT WIN32_LEAN_AND_MEAN
    _FILE_OFFSET_BITS=64
    TH_PS4_GAME_DIR="/data/touhou/th08"
    # TH08 steps into its own replay/ and backup/ subdirectories with chdir(), which has no
    # effect in the PS4 sandbox, so the shared glue keeps a virtual working directory.
    TH_PS4_VIRTUAL_CWD
)
target_compile_options(th08 PRIVATE
    -march=btver2 -fno-strict-aliasing
    # Keeps frame pointers in the optimized adapter too, so crash backtraces stay usable.
    -fno-omit-frame-pointer
    -Wno-gnu-anonymous-struct -Wno-unused-command-line-argument -Wno-write-strings
    # The reconstruction reproduces VC7 code that passes the address of a temporary to
    # __fastcall helpers; GCC accepts it under -fpermissive, and this is Clang's equivalent.
    -Wno-address-of-temporary -Wno-writable-strings
    "SHELL:-include ${PS4_COMMON_DIR}/ps4_compat.hpp"
    "SHELL:-include ${TH08_DIR}/src/modern/ps4/th08_ps4_compat.hpp"
)
# The reconstructed game TUs keep the original VC7 /Od boundaries: their inline bodies are
# not available across translation units, and optimizing them changes the reconstruction's
# own layout assumptions. The handwritten adapter is a hot path and is optimized.
set_source_files_properties(${TH08_GAME_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")
set_source_files_properties(${TH08_PS4_ADAPTER_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2_PS4 REQUIRED IMPORTED_TARGET GLOBAL sdl2 SDL2_image SDL2_ttf)
# The shared adapter includes <SDL.h> unprefixed, as the Linux target does.
target_include_directories(th08 PRIVATE ${SDL2_PS4_INCLUDE_DIRS})
target_link_libraries(th08 PRIVATE ${SDL2_PS4_STATIC_LDFLAGS}
    SceVideoOut SceUserService SceSysmodule SceGnmDriver SceSystemService)
target_link_options(th08 PRIVATE "LINKER:--wrap=fopen" "LINKER:--wrap=mkdir"
    "LINKER:--wrap=chdir" "LINKER:--wrap=rename")
foreach(fn malloc free calloc realloc aligned_alloc memalign posix_memalign)
    target_link_options(th08 PRIVATE "LINKER:--wrap=${fn}")
endforeach()

set(OPENGNM_STACK "$ENV{HOME}/opengnm-stack" CACHE PATH "PS4-OpenGNM stack checkout")
set(PSBC "$ENV{HOME}/old-psbc/zig-out/bin/psbc" CACHE FILEPATH "PS4 shader binary compiler")
set(EX ${OPENGNM_STACK}/freegnm-examples)
target_sources(th08 PRIVATE
    ${EX}/shared/src/u/fs.c ${EX}/shared/src/u/utility.c ${EX}/shared/src/displayctx.c
    ${EX}/shared/src/memalloc.c ${EX}/shared/src/misc.c)
set_source_files_properties(
    ${EX}/shared/src/u/fs.c ${EX}/shared/src/u/utility.c ${EX}/shared/src/displayctx.c
    ${EX}/shared/src/memalloc.c ${EX}/shared/src/misc.c
    PROPERTIES COMPILE_OPTIONS "-std=c11;-D_BSD_SOURCE;-DUSE_OPENGNM")
target_include_directories(th08 BEFORE PRIVATE
    ${OPENGNM_STACK}/opengnm/include ${OPENGNM_STACK}/opengnm/include/gnm ${EX}/shared/src)
target_link_libraries(th08 PRIVATE ${OPENGNM_STACK}/opengnm/libopengnm.a)

# GLSL -> SPIR-V -> PS4 shader binary. The older psbc is deliberate: unlike opengnm-psbc it
# keeps the descriptor-table pointers used by this renderer.
set(TH_SHADERS)
foreach(pair "th08_gnm.vert:vertex:${TH08_DIR}/src/modern/ps4"
             "th08_gnm.frag:fragment:${TH08_DIR}/src/modern/ps4"
             "clear.frag:fragment:${EX}/triangle/assets/misc")
    string(REPLACE ":" ";" parts ${pair})
    list(GET parts 0 name)
    list(GET parts 1 stage)
    list(GET parts 2 dir)
    set(out ${CMAKE_BINARY_DIR}/pkg/assets/misc/${name}.sb)
    add_custom_command(OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pkg/assets/misc
        COMMAND glslc -fshader-stage=${stage} ${dir}/${name}.glsl -o ${CMAKE_BINARY_DIR}/${name}.spv
        COMMAND ${PSBC} -s ${stage} ${CMAKE_BINARY_DIR}/${name}.spv ${out}
        DEPENDS ${dir}/${name}.glsl)
    list(APPEND TH_SHADERS ${out})
endforeach()
add_custom_target(th08_shaders ALL DEPENDS ${TH_SHADERS})

set(TH_GAME_ID th08)
include(${PS4_COMMON_DIR}/pkg.cmake)
th_prepare_pkg_dir(${CMAKE_BINARY_DIR}/pkg ${CMAKE_CURRENT_SOURCE_DIR}/pkg th08.dat thbgm.dat)

include(${PS4_COMMON_DIR}/self.cmake)
th_add_self(th08 GAME)
add_pkg(th08 ${CMAKE_BINARY_DIR}/pkg "THSP00008" "Touhou 8 - Imperishable Night" "01.00" 0 gd)
add_dependencies(th08_pkg th08_shaders)
