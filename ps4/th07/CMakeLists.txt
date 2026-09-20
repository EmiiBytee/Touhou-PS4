cmake_minimum_required(VERSION 3.20)

# PS4 build of th07-portable. This is a full game using GNM directly; Piglet only works
# for mini apps and would disable the console's gameplay recording.
project(th07_ps4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TH07_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/th07)
set(PS4_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

set(TH07_SOURCES
    AnmManager.cpp AnmVm.cpp AsciiManager.cpp BombData.cpp BulletManager.cpp
    Chain.cpp Controller.cpp EclManager.cpp EffectManager.cpp Ending.cpp
    EnemyEclInstr.cpp EnemyManager.cpp FileSystem.cpp GameErrorContext.cpp
    GameManager.cpp GameWindow.cpp Gui.cpp ItemManager.cpp MainMenu.cpp
    MidiOutput.cpp MusicRoom.cpp Player.cpp ReplayManager.cpp ResultScreen.cpp
    Rng.cpp ScreenEffect.cpp SoundPlayer.cpp Stage.cpp Supervisor.cpp
    TextHelper.cpp main.cpp utils.cpp graphics/Gnm.cpp
    pbg4/Lzss.cpp pbg4/Pbg4Archive.cpp pbg4/Pbg4File.cpp
)
list(TRANSFORM TH07_SOURCES PREPEND ${TH07_DIR}/src/)

add_executable(th07 ${TH07_SOURCES}
    ${PS4_COMMON_DIR}/ps4_platform.cpp
    ${PS4_COMMON_DIR}/ps4_keyboard.cpp
    ${PS4_COMMON_DIR}/ps4_malloc.cpp
    ${PS4_COMMON_DIR}/thpatch/thpatch.cpp
    ${PS4_COMMON_DIR}/thpatch/json.cpp
)
target_include_directories(th07 PRIVATE
    ${TH07_DIR}/src ${TH07_DIR}/src/pbg4 ${PS4_COMMON_DIR} ${PS4_COMMON_DIR}/thpatch)
target_compile_definitions(th07 PRIVATE
    NDEBUG TH_PS4 TH_PS4_BIG_APP TH_PS4_GNM TH_THPATCH
    TH_PS4_GAME_DIR="/data/touhou/th07"
    # SDL owns PS4 AudioOut; miniaudio remains the game's mixer/decoder and is pulled from
    # SDL's callback instead of opening an unsupported native backend.
    MA_NO_DEVICE_IO
)
target_compile_options(th07 PRIVATE
    -O2 -march=btver2 -Wno-gnu-anonymous-struct -Wno-unused-command-line-argument
    "SHELL:-include ${PS4_COMMON_DIR}/ps4_compat.hpp"
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2_PS4 REQUIRED IMPORTED_TARGET GLOBAL sdl2 SDL2_image SDL2_ttf)
target_link_libraries(th07 PRIVATE ${SDL2_PS4_STATIC_LDFLAGS}
    SceVideoOut SceUserService SceSysmodule SceGnmDriver SceSystemService)
target_link_options(th07 PRIVATE "LINKER:--wrap=fopen" "LINKER:--wrap=mkdir")
foreach(fn malloc free calloc realloc aligned_alloc memalign posix_memalign)
    target_link_options(th07 PRIVATE "LINKER:--wrap=${fn}")
endforeach()

set(OPENGNM_STACK "$ENV{HOME}/opengnm-stack" CACHE PATH "PS4-OpenGNM stack checkout")
set(PSBC "$ENV{HOME}/old-psbc/zig-out/bin/psbc" CACHE FILEPATH "PS4 shader binary compiler")
set(EX ${OPENGNM_STACK}/freegnm-examples)
target_sources(th07 PRIVATE
    ${EX}/shared/src/u/fs.c ${EX}/shared/src/u/utility.c ${EX}/shared/src/displayctx.c
    ${EX}/shared/src/memalloc.c ${EX}/shared/src/misc.c)
set_source_files_properties(
    ${EX}/shared/src/u/fs.c ${EX}/shared/src/u/utility.c ${EX}/shared/src/displayctx.c
    ${EX}/shared/src/memalloc.c ${EX}/shared/src/misc.c
    PROPERTIES COMPILE_OPTIONS "-std=c11;-D_BSD_SOURCE;-DUSE_OPENGNM")
target_include_directories(th07 BEFORE PRIVATE
    ${OPENGNM_STACK}/opengnm/include ${OPENGNM_STACK}/opengnm/include/gnm ${EX}/shared/src)
target_link_libraries(th07 PRIVATE ${OPENGNM_STACK}/opengnm/libopengnm.a)

# GLSL -> SPIR-V -> PS4 shader binary. The older psbc is deliberate: unlike
# opengnm-psbc it keeps the descriptor-table pointers used by this renderer.
set(TH_SHADERS)
foreach(pair "th_gnm.vert:vertex:${TH07_DIR}/src/graphics"
                 "th_gnm.frag:fragment:${TH07_DIR}/src/graphics"
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
add_custom_target(th07_shaders ALL DEPENDS ${TH_SHADERS})

set(TH_GAME_ID th07)
include(${PS4_COMMON_DIR}/pkg.cmake)
th_prepare_pkg_dir(${CMAKE_BINARY_DIR}/pkg ${CMAKE_CURRENT_SOURCE_DIR}/pkg th07.dat thbgm.dat)

include(${PS4_COMMON_DIR}/self.cmake)
th_add_self(th07 GAME)
add_pkg(th07 ${CMAKE_BINARY_DIR}/pkg "THSP00007" "Touhou 7 - Perfect Cherry Blossom" "01.00" 0 gd)
add_dependencies(th07_pkg th07_shaders)
