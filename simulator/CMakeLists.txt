cmake_minimum_required(VERSION 3.20)
project(umami_marquee_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(UNICORN REQUIRED IMPORTED_TARGET unicorn)
pkg_check_modules(CAPSTONE REQUIRED IMPORTED_TARGET capstone)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebpdemux)
find_package(SDL3 REQUIRED CONFIG)

find_package(ZLIB REQUIRED)

set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/imgui)
set(THIRD ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# the simulator core: the board model and the symbols
add_library(simcore STATIC src/board.cpp src/symbols.cpp src/attract_lines.cpp)
target_include_directories(simcore PUBLIC src)
target_link_libraries(simcore PUBLIC PkgConfig::UNICORN)
target_compile_options(simcore PRIVATE -Wall -Wextra)

# host-side pieces: the real sign's serial link and debug record (the Sign window), the display models (--model),
# and the references the firmware checks compare against (native emoji, the firmware's fonts, RS-485 records).
# Umami Marquee Studio itself is the web app (frontend/apps/studio + studio/server.py), not C++.
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS src/host/*.cpp)
add_library(hostcore STATIC ${HOST_SOURCES})
target_include_directories(hostcore PUBLIC src/host)
target_include_directories(hostcore SYSTEM PUBLIC ${THIRD}/stb ${THIRD}/nlohmann)
target_link_libraries(hostcore PUBLIC ZLIB::ZLIB PkgConfig::WEBP)
target_compile_options(hostcore PRIVATE -Wall -Wextra)

# run a boot-flash image from RAM on the sign over the DB9 (the Studio's sign link); tests/test_signlink.sh
add_executable(sign-link-run tests/signlink_run.cpp)
target_link_libraries(sign-link-run PRIVATE hostcore)
target_include_directories(sign-link-run PRIVATE src)

# headless runs and run fingerprints
add_executable(umami-marquee-sim-check src/headless.cpp)
target_link_libraries(umami-marquee-sim-check PRIVATE simcore)

# the verification runs (the port of sim/compare*.py, flashtest.py, find_culprit.py, test_*.py)
add_executable(umami-marquee-sim-test src/simtest.cpp src/simtest_emoji.cpp)
target_link_libraries(umami-marquee-sim-test PRIVATE simcore hostcore)
target_compile_options(umami-marquee-sim-test PRIVATE -Wall -Wextra)

# an ELF with the firmware's names, for gdb and VS Code
add_executable(umami-marquee-sim-elf src/elf.cpp)
target_link_libraries(umami-marquee-sim-elf PRIVATE simcore)

# the debugger: Dear ImGui on SDL3
add_executable(umami-marquee-sim src/app.cpp src/pty_bridge.cpp src/sign_ui.cpp src/led_player.cpp src/ui_style.cpp src/timeline.cpp src/machine.cpp src/gdbserver.cpp src/disasm.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(umami-marquee-sim PRIVATE src ${IMGUI} ${IMGUI}/backends ${THIRD}/fonts-ui)
target_link_libraries(umami-marquee-sim PRIVATE simcore hostcore SDL3::SDL3 PkgConfig::CAPSTONE)

# the from-source firmware in the simulator, without a window (tools/fw_check.sh)
set_source_files_properties(../firmware/app/umframe.c PROPERTIES LANGUAGE CXX)
foreach(t fw_run fw_reboot fw_umframe)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src ../firmware/app)
  target_compile_options(${t} PRIVATE -Dprivate=public)
  target_link_libraries(${t} PRIVATE simcore ZLIB::ZLIB)
endforeach()
target_sources(fw_umframe PRIVATE ../firmware/app/umframe.c)
