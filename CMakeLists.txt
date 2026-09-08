cmake_minimum_required(VERSION 3.16)
project(gameboy C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(GB_ENABLE_TUI "Build the ncurses debugger frontend" ON)

add_library(gb STATIC src/core/gb.c src/core/audio.c src/core/ppu.c src/core/opcodes.c)
target_include_directories(gb PUBLIC src/core)
target_compile_options(gb PRIVATE -Wall -Wextra -Werror)

if(EMSCRIPTEN)
  # Web build: core library plus the browser bindings. SDL, ncurses and the
  # native test runners are desktop-only.
  add_executable(gb-wasm src/frontend/wasm/main.c)
  target_link_libraries(gb-wasm PRIVATE gb)
  target_compile_options(gb-wasm PRIVATE -Wall -Wextra -Werror)
  target_link_options(gb-wasm PRIVATE
    "-sWASM=1"
    "-sSINGLE_FILE=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sNO_EXIT_RUNTIME=1"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP16','HEAPU32']"
    "-sEXPORTED_FUNCTIONS=['_main','_wasm_boot','_wasm_free','_wasm_load','_wasm_logo_valid','_wasm_run_frame','_wasm_framebuffer','_wasm_set_input','_wasm_save_size','_wasm_save','_wasm_load_save','_wasm_state_size','_wasm_state_save','_wasm_state_load','_wasm_reset','_wasm_audio_available','_wasm_audio_ptr','_wasm_audio_consume','_malloc','_free']"
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/site/index.html")
  set_target_properties(gb-wasm PROPERTIES SUFFIX ".html")
  add_custom_command(TARGET gb-wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/site/app.js $<TARGET_FILE_DIR:gb-wasm>/app.js)
  # Node-targeted twin of the same bindings for automated smoke testing.
  add_executable(gb-wasm-node src/frontend/wasm/main.c)
  target_link_libraries(gb-wasm-node PRIVATE gb)
  target_compile_options(gb-wasm-node PRIVATE -Wall -Wextra -Werror)
  target_link_options(gb-wasm-node PRIVATE
    "-sWASM=1"
    "-sSINGLE_FILE=1"
    "-sENVIRONMENT=node"
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=createGameBoy"
    "-sNO_EXIT_RUNTIME=1"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP16','HEAPU32']"
    "-sEXPORTED_FUNCTIONS=['_main','_wasm_boot','_wasm_free','_wasm_load','_wasm_logo_valid','_wasm_run_frame','_wasm_framebuffer','_wasm_set_input','_wasm_save_size','_wasm_save','_wasm_load_save','_wasm_state_size','_wasm_state_save','_wasm_state_load','_wasm_reset','_wasm_audio_available','_wasm_audio_ptr','_wasm_audio_consume','_malloc','_free']")
  return()
endif()

add_executable(gb-headless src/frontend/headless/main.c)
target_link_libraries(gb-headless PRIVATE gb)
target_compile_options(gb-headless PRIVATE -Wall -Wextra -Werror)

add_executable(gb-link src/frontend/link/main.c)
target_link_libraries(gb-link PRIVATE gb)
target_compile_options(gb-link PRIVATE -Wall -Wextra -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
add_executable(gb-sdl src/frontend/sdl/main.c src/frontend/sdl/input.c
  src/frontend/sdl/ui_assets.c src/frontend/sdl/config.c)
target_include_directories(gb-sdl PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(gb-sdl PRIVATE gb ${SDL2_LIBRARIES})
target_compile_options(gb-sdl PRIVATE -Wall -Wextra -Werror ${SDL2_CFLAGS_OTHER})
configure_file(src/frontend/sdl/ui_assets.txt ui_assets.txt COPYONLY)

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
add_test(NAME blargg COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_blargg.sh
  $<TARGET_FILE:gb-headless>)
add_test(NAME mooneye COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mooneye.sh
  $<TARGET_FILE:gb-headless>)
add_test(NAME same_suite COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_same_suite.sh
  $<TARGET_FILE:gb-headless>)
add_test(NAME sound COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_sound.sh
  $<TARGET_FILE:gb-headless>)

add_custom_target(gb-wasm
  COMMAND ${CMAKE_COMMAND} -E echo "Emscripten required: emcmake cmake -S . -B build-wasm"
  COMMENT "WASM target unavailable in this toolchain")
