cmake_minimum_required(VERSION 3.20)
project(megadrive_raylib C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------- raylib ---
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
# fetched as a tarball (not a git clone) so no nested .git confuses VCS tools;
# version- and integrity-pinned for reproducible builds
FetchContent_Declare(
  raylib
  URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
  URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
)
FetchContent_MakeAvailable(raylib)

set(VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor)

# ------------------------------------- Musashi opcode-handler generation ---
# m68kmake is a host tool that turns m68k_in.c into m68kops.c/.h
add_executable(m68kmake ${VENDOR}/musashi/m68kmake.c)
set(MUSASHI_GEN ${CMAKE_CURRENT_BINARY_DIR}/musashi_gen)
file(MAKE_DIRECTORY ${MUSASHI_GEN})
add_custom_command(
  OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h
  COMMAND m68kmake ${MUSASHI_GEN} ${VENDOR}/musashi/m68k_in.c
  DEPENDS m68kmake ${VENDOR}/musashi/m68k_in.c ${VENDOR}/musashi/m68kconf.h
  COMMENT "Generating Musashi 68000 opcode handlers"
)

# ------------------------------------------------------------- emulator ----
add_executable(mdplayer
  src/main.c
  src/core/md.c
  src/core/vdp.c
  src/core/io.c
  src/core/psg.c
  src/core/z80bus.c
  ${VENDOR}/musashi/m68kcpu.c
  ${VENDOR}/musashi/m68kdasm.c
  ${VENDOR}/musashi/softfloat/softfloat.c
  ${MUSASHI_GEN}/m68kops.c
  ${VENDOR}/z80/z80.c
  ${VENDOR}/nuked-opn2/ym3438.c
)

target_include_directories(mdplayer PRIVATE
  src
  ${VENDOR}/musashi
  ${MUSASHI_GEN}
  ${VENDOR}/z80
  ${VENDOR}/nuked-opn2
)

target_link_libraries(mdplayer PRIVATE raylib)

if(MINGW)
  target_link_libraries(mdplayer PRIVATE winmm)
endif()
