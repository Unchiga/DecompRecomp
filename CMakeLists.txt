cmake_minimum_required(VERSION 3.21)
project(memories_pc LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
include(CTest)
option(MEMORIES_SANITIZERS "Enable host address/undefined behavior checks" OFF)
set(MEMORIES_PSYZ_ROOT "" CACHE PATH "Optional pinned PSY-Z checkout (repository root)")

add_library(memories_pc_core STATIC
    src/pc/compat/gte.c
    src/pc/compat/libgs_ot.c
    src/pc/memory.c
    src/pc/render/packets.c
    src/pc/render/soft_gpu.c
    src/pc/rng.c
    src/game/rand_get_interval.c
    src/game/util_compare_s16.c)
target_include_directories(memories_pc_core PUBLIC src)
# Only the game unit sees these aliases; host libc and the SDK keep their names.
set_source_files_properties(src/game/rand_get_interval.c PROPERTIES
    COMPILE_DEFINITIONS "rand=Memories_Rand;srand=Memories_Srand")
if(MSVC)
    target_compile_options(memories_pc_core PRIVATE /W4 /WX)
else()
    target_compile_options(memories_pc_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MEMORIES_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "Use GCC or Clang for MEMORIES_SANITIZERS")
    endif()
    target_compile_options(memories_pc_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(memories_pc_core PUBLIC -fsanitize=address,undefined)
endif()

if(BUILD_TESTING)
    add_executable(memories_pc_core_test tests/pc/core_test.c)
    target_link_libraries(memories_pc_core_test PRIVATE memories_pc_core)
    add_test(NAME pc_core COMMAND memories_pc_core_test)
    add_executable(memories_packet_test tests/pc/packet_test.c)
    target_link_libraries(memories_packet_test PRIVATE memories_pc_core)
    add_test(NAME pc_packets COMMAND memories_packet_test)
    add_executable(memories_libgs_ot_test tests/pc/libgs_ot_test.c)
    target_link_libraries(memories_libgs_ot_test PRIVATE memories_pc_core)
    add_test(NAME pc_libgs_ot COMMAND memories_libgs_ot_test)
    add_executable(memories_gte_test tests/pc/gte_test.c)
    target_link_libraries(memories_gte_test PRIVATE memories_pc_core)
    add_test(NAME pc_gte COMMAND memories_gte_test)
    add_executable(memories_soft_gpu_test tests/pc/soft_gpu_test.c)
    target_link_libraries(memories_soft_gpu_test PRIVATE memories_pc_core)
    add_test(NAME pc_soft_gpu COMMAND memories_soft_gpu_test)
    add_executable(memories_settings_test tests/pc/settings_test.c src/pc/platform/settings.c)
    target_include_directories(memories_settings_test PRIVATE src)
    if(NOT MSVC)
        target_compile_options(memories_settings_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME pc_settings COMMAND memories_settings_test)
    find_package(Threads REQUIRED)
    add_executable(memories_log_test tests/pc/log_test.c src/pc/debug/log.c)
    target_include_directories(memories_log_test PRIVATE src)
    target_link_libraries(memories_log_test PRIVATE Threads::Threads)
    if(NOT MSVC)
        target_compile_options(memories_log_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME pc_log COMMAND memories_log_test)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(pc_audit
        COMMAND "${Python3_EXECUTABLE}" tools/pc/audit.py
            --output "${CMAKE_BINARY_DIR}/port-audit.json"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Inventory native-port blockers and SDK references")
endif()

if(MEMORIES_PSYZ_ROOT)
    find_package(Git REQUIRED)
    file(READ "${CMAKE_SOURCE_DIR}/config/pc/dependencies.json" pc_dependencies)
    string(JSON psyz_revision GET "${pc_dependencies}" psyz revision)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${MEMORIES_PSYZ_ROOT}" rev-parse HEAD
        OUTPUT_VARIABLE psyz_actual OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE psyz_git_result)
    if(NOT psyz_git_result EQUAL 0 OR NOT psyz_actual STREQUAL psyz_revision)
        message(FATAL_ERROR "PSY-Z must be checked out at ${psyz_revision}; see notes/pc-build.md")
    endif()
    if(NOT EXISTS "${MEMORIES_PSYZ_ROOT}/external/SDL/CMakeLists.txt")
        message(FATAL_ERROR "Initialize PSY-Z's external/SDL submodule; see notes/pc-build.md")
    endif()
    enable_language(CXX)
    # PSY-Z uses BSD type aliases and requires its own C dialect.
    set(CMAKE_C_EXTENSIONS ON)
    add_subdirectory("${MEMORIES_PSYZ_ROOT}/psyz" "${CMAKE_BINARY_DIR}/psyz" EXCLUDE_FROM_ALL)
    add_executable(memories_psyz_probe tests/pc/psyz_probe.c)
    target_link_libraries(memories_psyz_probe PRIVATE psyz)
    if(UNIX)
        target_link_libraries(memories_psyz_probe PRIVATE m)
    endif()
    if(BUILD_TESTING)
        add_test(NAME psyz_contract_probe COMMAND memories_psyz_probe)
    endif()
    add_library(memories_psyz_gpu STATIC src/pc/render/psyz_gpu.c)
    target_link_libraries(memories_psyz_gpu PUBLIC memories_pc_core psyz)
    if(UNIX)
        target_link_libraries(memories_psyz_gpu PUBLIC m)
    endif()
    add_executable(memories_render_smoke tests/pc/render_smoke.c)
    target_link_libraries(memories_render_smoke PRIVATE memories_psyz_gpu)
    add_executable(memories-pc-preview tests/pc/render_smoke.c)
    target_compile_definitions(memories-pc-preview PRIVATE MEMORIES_PREVIEW=1)
    target_link_libraries(memories-pc-preview PRIVATE memories_psyz_gpu)
    install(TARGETS memories-pc-preview RUNTIME DESTINATION .)
    # Requires a working graphics driver; core/contract tests remain headless.
    option(MEMORIES_RENDER_TESTS "Register the graphics-driver-dependent smoke test" OFF)
    if(BUILD_TESTING AND MEMORIES_RENDER_TESTS)
        add_test(NAME pc_render COMMAND memories_render_smoke)
    endif()
endif()
