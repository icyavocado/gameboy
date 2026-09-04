cmake_minimum_required(VERSION 3.16)
project(gameboy C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(GB_ENABLE_TUI "Build the ncurses debugger frontend" ON)

add_library(gb STATIC src/core/gb.c src/core/opcodes.c)
target_include_directories(gb PUBLIC src/core)
target_compile_options(gb PRIVATE -Wall -Wextra -Werror)

add_executable(gb-headless src/frontend/headless/main.c)
target_link_libraries(gb-headless PRIVATE gb)
target_compile_options(gb-headless PRIVATE -Wall -Wextra -Werror)

add_executable(gb-link src/frontend/link/main.c)
target_link_libraries(gb-link PRIVATE gb)
target_compile_options(gb-link PRIVATE -Wall -Wextra -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
add_executable(gb-sdl src/frontend/sdl/main.c src/frontend/sdl/input.c)
target_include_directories(gb-sdl PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(gb-sdl PRIVATE gb ${SDL2_LIBRARIES})
target_compile_options(gb-sdl PRIVATE -Wall -Wextra -Werror ${SDL2_CFLAGS_OTHER})

if(GB_ENABLE_TUI)
  pkg_check_modules(NCURSESW REQUIRED ncursesw)
  target_compile_definitions(gb-sdl PRIVATE GB_ENABLE_TUI)
  target_include_directories(gb-sdl PRIVATE ${NCURSESW_INCLUDE_DIRS})
  target_link_libraries(gb-sdl PRIVATE ${NCURSESW_LIBRARIES})
  add_executable(gb-tui src/frontend/tui/main.c)
  target_include_directories(gb-tui PRIVATE ${NCURSESW_INCLUDE_DIRS})
  target_link_libraries(gb-tui PRIVATE gb ${NCURSESW_LIBRARIES})
  target_compile_options(gb-tui PRIVATE -Wall -Wextra -Werror ${NCURSESW_CFLAGS_OTHER})
endif()

enable_testing()
add_executable(gb-test tests/test_core.c)
target_include_directories(gb-test PRIVATE tests/unit)
target_link_libraries(gb-test PRIVATE gb)
target_compile_options(gb-test PRIVATE -Wall -Wextra -Werror)
add_test(NAME core COMMAND gb-test)

add_executable(gb-input-test tests/test_input.c src/frontend/sdl/input.c)
target_include_directories(gb-input-test PRIVATE src/frontend/sdl ${SDL2_INCLUDE_DIRS})
target_link_libraries(gb-input-test PRIVATE gb)
target_compile_options(gb-input-test PRIVATE -Wall -Wextra -Werror ${SDL2_CFLAGS_OTHER})
add_test(NAME input COMMAND gb-input-test)

add_executable(gb-gameplay-test tests/test_gameplay.c)
target_link_libraries(gb-gameplay-test PRIVATE gb)
target_compile_options(gb-gameplay-test PRIVATE -Wall -Wextra -Werror)
add_test(NAME gameplay COMMAND gb-gameplay-test)

if(EMSCRIPTEN)
  add_executable(gb-wasm src/frontend/wasm/main.c)
  target_link_libraries(gb-wasm PRIVATE gb)
else()
  add_custom_target(gb-wasm
    COMMAND ${CMAKE_COMMAND} -E echo "Emscripten required: emcmake cmake -S . -B build-wasm"
    COMMENT "WASM target unavailable in this toolchain")
endif()
