# Offline test for the PC SETTINGS tab in R3's own Options menu (project/r3_menurows.cpp).
# Fake guest memory + fake GFx array functions (func_0032F0B4 / func_009FCAE0 / func_009FBC6C), a fake
# Options menu behind the menu Invoke (func_0032D788) and
# fake settings providers (pcm_* / pcx_*); does not need the game or the rest of the lifted code
# (only recompiled/ppu_recomp.h for the ppu_context layout).
#   cmake -S project/tests/menu_rows -B build_menurowstest -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl
#   cmake --build build_menurowstest && ctest --test-dir build_menurowstest --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(menu_rows_test CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(PORT_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(PORT_ROOT "${PORT_PROJECT_DIR}/.." ABSOLUTE)

add_executable(menu_rows_test menu_rows_test.cpp "${PORT_PROJECT_DIR}/r3_menurows.cpp")
target_include_directories(menu_rows_test PRIVATE "${PORT_ROOT}/recompiled")
target_compile_definitions(menu_rows_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX PPU_NO_INLINE_LOADS PPU_NO_INLINE_STORES)

add_test(NAME menu_rows_unit COMMAND menu_rows_test unit)
add_test(NAME menu_rows_frontend COMMAND menu_rows_test frontend)
set_tests_properties(menu_rows_frontend PROPERTIES ENVIRONMENT "R3_MENU_ROWS=")
add_test(NAME menu_rows_off COMMAND menu_rows_test off)
add_test(NAME menu_rows_controls COMMAND menu_rows_test controls)
set_tests_properties(menu_rows_unit PROPERTIES ENVIRONMENT "R3_MENU_ROWS=")
set_tests_properties(menu_rows_off PROPERTIES ENVIRONMENT "R3_MENU_ROWS=0")
set_tests_properties(menu_rows_controls PROPERTIES ENVIRONMENT "R3_MENU_ROWS=2")
