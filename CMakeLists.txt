cmake_minimum_required(VERSION 3.25)
# macOS: what players run (the runtime) is built for macOS 10.15, the oldest
# macOS a Vulkan 1.3 MoltenVK runs on; so runtime code avoids std::format,
# which Apple's libc++ only has from 13.3. The developer tools (qilin, the
# tests) need 13.3 and are built for it.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15 CACHE STRING "oldest macOS the runtime runs on")
endif()
# The single source of the version number (see CHANGELOG.md for the scheme).
project(MarioKartDDWiiQilinRecomp VERSION 0.22.0 LANGUAGES C CXX)

# ---- Language: C++23 everywhere (recompiler, runtime, generated code) ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Minimum compilers for the C++23 language and library features we use:
# GCC 14, Clang 19, Xcode 16 (AppleClang 16), Visual Studio 2022 17.10
# (std::expected, std::print, std::ranges, std::string::contains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old: C++23 <print> needs GCC 14 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Clang 18 lacks the C++20 concepts level libstdc++ requires for std::expected.
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Clang 19 or newer")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old: needs Xcode 16 or newer")
elseif(MSVC AND MSVC_VERSION LESS 1940)
    message(FATAL_ERROR "MSVC ${MSVC_VERSION} is too old: needs Visual Studio 2022 17.10 or newer")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "64-bit hosts only (see docs/roadmap-and-design-targets.md)")
endif()

option(QILIN_WERROR "Treat warnings in our own code as errors" OFF)

# Floating point is bit-exact with the guest: the host compiler must not fuse
# a multiply and an add on its own (Broadway's fmadd family is fused, and is
# written as std::fma; nothing else is).
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

# Sanitizers (GCC and Clang): -DQILIN_SANITIZE=address,undefined builds everything with them,
# and the first report stops the program. The code lifted from the game is left out unless
# QILIN_SANITIZE_LIFTED is on (32 large files: several times the build time); the lifter's output
# is covered either way by the differential test, whose lifted code always gets them.
set(QILIN_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined (GCC, Clang)")
option(QILIN_SANITIZE_LIFTED "With QILIN_SANITIZE: the game's lifted code too" OFF)
if(QILIN_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "QILIN_SANITIZE is for GCC and Clang")
    endif()
    add_compile_options(-fsanitize=${QILIN_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${QILIN_SANITIZE})
endif()

# For this machine's own CPU (a build that runs only here, as
# tools/build-mkdd.sh makes): the Raspberry Pi 4's Cortex-A72, say,
# instead of the baseline ARMv8.0 or x86-64. Off by default (CI, downloads).
option(QILIN_NATIVE "Optimise for the CPU of the machine building (not portable)" OFF)
if(QILIN_NATIVE AND NOT MSVC)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        add_compile_options(-mcpu=native)
    else()
        add_compile_options(-march=native)
    endif()
elseif(QILIN_NATIVE AND MSVC)
    # MSVC has no "native": /arch:AVX2 (with FMA, for the Wii's fused
    # multiply-adds) when this machine runs it.
    try_run(qilin_avx2_runs qilin_avx2_compiles ${CMAKE_BINARY_DIR}/cpu_avx2 ${CMAKE_SOURCE_DIR}/cmake/cpu_avx2.cpp)
    if(qilin_avx2_compiles AND qilin_avx2_runs EQUAL 0)
        add_compile_options(/arch:AVX2)
        message(STATUS "QILIN_NATIVE: /arch:AVX2")
    else()
        message(STATUS "QILIN_NATIVE: this CPU has no AVX2 and FMA; the baseline instructions")
    endif()
endif()

# ---- Warnings for our own targets ----
add_library(qilin_warnings INTERFACE)
if(MSVC)
    target_compile_options(qilin_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor)
    # fopen/strerror etc. are standard C++; MSVC's "use the _s variants" advice is not.
    target_compile_definitions(qilin_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qilin_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(QILIN_WERROR)
        target_compile_options(qilin_warnings INTERFACE -Werror)
    endif()
endif()

# Developer tools on macOS: macOS 13.3 or later (std::format).
function(qilin_tool target)
    if(APPLE)
        target_compile_options(${target} PRIVATE -mmacosx-version-min=13.3)
        target_link_options(${target} PRIVATE -mmacosx-version-min=13.3)
    endif()
endfunction()

# ---- The DOL loader: runtime code too (no std::format) ----
add_library(qilin_dol STATIC recomp/src/dol.cpp recomp/src/sha256.cpp)
target_include_directories(qilin_dol PUBLIC recomp/src)
target_link_libraries(qilin_dol PRIVATE qilin_warnings)

# ---- Recompiler core ----
add_library(qilin_core STATIC
    recomp/src/dol_json.cpp
    recomp/src/decoder.cpp
    recomp/src/cfg.cpp
    recomp/src/lift.cpp
    recomp/src/idle.cpp
)
target_include_directories(qilin_core PUBLIC recomp/src)
target_link_libraries(qilin_core PUBLIC qilin_dol PRIVATE qilin_warnings)
qilin_tool(qilin_core)

add_executable(qilin recomp/src/main.cpp)
target_link_libraries(qilin PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin)
target_compile_definitions(qilin PRIVATE QILIN_VERSION="${PROJECT_VERSION}")

# ---- Runtime (what players run: no std::format) ----
add_library(qilin_rt STATIC runtime/src/memory.cpp runtime/src/system.cpp runtime/src/report.cpp)
target_include_directories(qilin_rt PUBLIC runtime/include)
target_link_libraries(qilin_rt PRIVATE qilin_warnings)

enable_testing()

# ---- The disc: >>> the disc agent's block (docs/disc.md) ----
# A GameCube image (.iso, .gcm, .ciso) or extracted folder, read in place.
# Runtime code too (no std::format): the game reads its files through it.
add_library(qilin_disc STATIC disc/src/disc.cpp disc/src/file.cpp disc/src/tools.cpp)
target_include_directories(qilin_disc PUBLIC disc/include)
target_link_libraries(qilin_disc PRIVATE qilin_warnings)

# qilin-disc: info, list, extract, dol, verify (the launcher's and the build's way to main.dol).
add_executable(qilin_disc_tool disc/src/main.cpp)
set_target_properties(qilin_disc_tool PROPERTIES OUTPUT_NAME qilin-disc)
target_include_directories(qilin_disc_tool PRIVATE disc/src)
target_link_libraries(qilin_disc_tool PRIVATE qilin_disc qilin_dol qilin_warnings)
qilin_tool(qilin_disc_tool)

# Synthetic discs as image, CISO and folder: no game files needed.
add_executable(qilin_disc_tests disc/tests/tests.cpp)
target_link_libraries(qilin_disc_tests PRIVATE qilin_disc qilin_warnings)
qilin_tool(qilin_disc_tests)
add_test(NAME qilin_disc_tests COMMAND qilin_disc_tests)
set_tests_properties(qilin_disc_tests PROPERTIES TIMEOUT 120)
# ---- <<< the disc agent's block ----

# ---- The host platform: window, input and audio (SDL3), Vulkan ----
# Fetched at configure time, pinned, built statically: SDL3, the Vulkan headers
# and volk (which loads Vulkan at run time, so no SDK is needed to build or run).
option(QILIN_HOST "Build the host platform layer (window, Vulkan, input, audio)" ON)
if(QILIN_HOST)
    include(FetchContent)
    set(FETCHCONTENT_QUIET ON)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)  # synthetic input: not needed
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-3.4.20 GIT_SHALLOW ON)
    FetchContent_Declare(VulkanHeaders GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
                         GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON)
    # volk is one C file: fetched without its own CMake project (SOURCE_SUBDIR points nowhere).
    FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW ON
                         SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(SDL3 VulkanHeaders volk)
    add_library(qilin_volk STATIC ${volk_SOURCE_DIR}/volk.c)
    target_include_directories(qilin_volk PUBLIC ${volk_SOURCE_DIR})
    target_link_libraries(qilin_volk PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
    target_compile_definitions(qilin_volk PUBLIC VK_NO_PROTOTYPES)

    # Dear ImGui (MIT) for the settings menu, as the other Qilin recomps draw
    # theirs: the same commit (v1.93.0), its core and its SDL3 and Vulkan
    # backends, fetched and pinned. Its Vulkan backend loads its functions
    # through volk (no prototypes).
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
        URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114)
    FetchContent_MakeAvailable(imgui)
    add_library(qilin_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
    target_include_directories(qilin_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_compile_definitions(qilin_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES VK_NO_PROTOTYPES IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    target_link_libraries(qilin_imgui PUBLIC SDL3::SDL3-static Vulkan::Headers)
    if(MSVC)
        target_compile_options(qilin_imgui PRIVATE /W0)  # third-party code: its warnings aren't ours
    else()
        target_compile_options(qilin_imgui PRIVATE -w)
    endif()

    # SMAA's tables and AMD FSR 1's constants (both MIT), for the passes before the
    # picture is shown (host/src/post.cpp): the files named in host/shaders/post/third_party.txt,
    # each fetched once and checked against its SHA-256.
    set(QILIN_POST_SOURCES ${CMAKE_BINARY_DIR}/_deps/qilin_post)
    file(STRINGS host/shaders/post/third_party.txt post_pins REGEX "^[0-9a-f]")
    foreach(pin IN LISTS post_pins)
        separate_arguments(pin)
        list(GET pin 0 pin_hash)
        list(GET pin 1 pin_path)
        list(GET pin 2 pin_url)
        file(DOWNLOAD ${pin_url} ${QILIN_POST_SOURCES}/${pin_path} EXPECTED_HASH SHA256=${pin_hash} TLS_VERIFY ON)
    endforeach()

    # A GLSL compiler for tools/build_shaders.py (the SPIR-V is committed: only
    # someone changing a shader needs one). Off by default; with it on, the
    # qilin_shader_compiler target builds Khronos's, pinned, and the script finds it.
    option(QILIN_SHADER_COMPILER "Build glslang, to regenerate the committed SPIR-V" OFF)
    if(QILIN_SHADER_COMPILER)
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
        set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
        set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
        set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glslang GIT_REPOSITORY https://github.com/KhronosGroup/glslang GIT_TAG 15.4.0 GIT_SHALLOW ON
                             EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(glslang)
        add_custom_target(qilin_shader_compiler DEPENDS glslang-standalone)
    endif()

    add_library(qilin_host STATIC host/src/vulkan.cpp host/src/display.cpp host/src/post.cpp host/src/fsr_constants.cpp
                host/src/capture.cpp host/src/audio.cpp host/src/paths.cpp)
    target_include_directories(qilin_host PUBLIC host/include PRIVATE ${QILIN_POST_SOURCES})
    target_link_libraries(qilin_host PUBLIC qilin_volk PRIVATE SDL3::SDL3-static qilin_warnings)
    # Vulkan structures are written {sType} with the rest zeroed: that's the idiom, not a mistake.
    # (On the sources, so it comes after -Wextra on the command line.)
    if(NOT MSVC)
        set_source_files_properties(host/src/vulkan.cpp host/src/display.cpp host/src/post.cpp host/src/audio.cpp host/src/paths.cpp host/tests/vktest.cpp
                                    PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
    # AMD's header, built for the CPU: its warnings aren't ours.
    set_source_files_properties(host/src/fsr_constants.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")

    # The Vulkan layer's own test: a device, an offscreen frame, read back. Skips
    # (exit 77) where there's no Vulkan 1.3 device, as on CI runners.
    add_executable(qilin_vktest host/tests/vktest.cpp)
    target_link_libraries(qilin_vktest PRIVATE qilin_host qilin_warnings)
    add_test(NAME qilin_vktest COMMAND qilin_vktest)
    set_tests_properties(qilin_vktest PROPERTIES SKIP_RETURN_CODE 77)

    # A controller reaches the game (SDL's virtual gamepad). Needs a window:
    # skips where there's no display, as on CI runners.
    add_executable(qilin_padtest host/tests/padtest.cpp)
    target_link_libraries(qilin_padtest PRIVATE qilin_host SDL3::SDL3-static qilin_warnings)
    add_test(NAME qilin_padtest COMMAND qilin_padtest)
    set_tests_properties(qilin_padtest PROPERTIES SKIP_RETURN_CODE 77)
    # The game's renderer end to end, without the game: GX register writes through
    # the recorder and renderer, the frame read back (skipped without Vulkan 1.3).
    add_executable(mkdd_render_test mkdd/tests/render_test.cpp mkdd/src/recorder.cpp mkdd/src/texture.cpp
                   mkdd/src/renderer.cpp mkdd/src/priority.cpp)
    target_include_directories(mkdd_render_test PRIVATE mkdd/src)
    target_link_libraries(mkdd_render_test PRIVATE qilin_host qilin_rt qilin_warnings)
    qilin_tool(mkdd_render_test)
    if(NOT MSVC)  # Vulkan structures written {sType}, as in the host layer
        set_source_files_properties(mkdd/tests/render_test.cpp mkdd/src/renderer.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
    add_test(NAME mkdd_render_test COMMAND mkdd_render_test)
    set_tests_properties(mkdd_render_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    # The same checks on each pixel shader path: the ubershader alone, and
    # every draw specialised to its shape (compiled as it's needed).
    add_test(NAME mkdd_render_test_ubershader COMMAND mkdd_render_test)
    set_tests_properties(mkdd_render_test_ubershader PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT "MKDD_UBERSHADER=1")
    add_test(NAME mkdd_render_test_specialized COMMAND mkdd_render_test)
    set_tests_properties(mkdd_render_test_specialized PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "MKDD_SPECIALIZE_SYNC=1")
    # And with every vertex in the full layout (the compact one is the default).
    add_test(NAME mkdd_render_test_full_vertices COMMAND mkdd_render_test)
    set_tests_properties(mkdd_render_test_full_vertices PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 ENVIRONMENT "MKDD_FULL_VERTICES=1")
endif()

# ---- Tests (no game files needed) ----
add_executable(qilin_tests recomp/tests/tests.cpp)
target_link_libraries(qilin_tests PRIVATE qilin_core qilin_rt qilin_warnings)
qilin_tool(qilin_tests)
add_test(NAME qilin_tests COMMAND qilin_tests)
add_executable(qilin_frame_times_test host/tests/frame_times_test.cpp)
target_include_directories(qilin_frame_times_test PRIVATE host/include)
target_link_libraries(qilin_frame_times_test PRIVATE qilin_warnings)
qilin_tool(qilin_frame_times_test)
add_test(NAME qilin_frame_times_test COMMAND qilin_frame_times_test)
# A capture's files: the PNG and tar writers (no window, no GPU).
add_executable(qilin_capture_test host/tests/capture_test.cpp host/src/capture.cpp)
target_include_directories(qilin_capture_test PRIVATE host/include)
target_link_libraries(qilin_capture_test PRIVATE qilin_warnings)
qilin_tool(qilin_capture_test)
add_test(NAME qilin_capture_test COMMAND qilin_capture_test)

# The thread model (the game's scheduler and interrupt layer) with guest
# functions written in C++: no game files needed.
add_executable(mkdd_threads_test mkdd/tests/threads_test.cpp mkdd/src/threads.cpp mkdd/src/os.cpp mkdd/src/restart.cpp mkdd/src/priority.cpp)
target_include_directories(mkdd_threads_test PRIVATE mkdd/src)
target_link_libraries(mkdd_threads_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_threads_test)
add_test(NAME mkdd_threads_test COMMAND mkdd_threads_test)
set_tests_properties(mkdd_threads_test PROPERTIES TIMEOUT 30)
add_test(NAME mkdd_threads_test_shutdown COMMAND mkdd_threads_test)
set_tests_properties(mkdd_threads_test_shutdown PROPERTIES TIMEOUT 30 ENVIRONMENT THREADS_TEST_LEAVE=shutdown
                     PASS_REGULAR_EXPRESSION "the game chose to power off: closing")
add_test(NAME mkdd_threads_test_restart COMMAND mkdd_threads_test)
set_tests_properties(mkdd_threads_test_restart PROPERTIES TIMEOUT 30 ENVIRONMENT THREADS_TEST_LEAVE=restart
                     PASS_REGULAR_EXPRESSION "OSRestart started the program again(.|\n)*all passed")

# The settings file's reader and writer.
add_executable(mkdd_settings_test mkdd/tests/settings_test.cpp mkdd/src/settings.cpp)
target_include_directories(mkdd_settings_test PRIVATE mkdd/src host/include)
target_link_libraries(mkdd_settings_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_settings_test)
add_test(NAME mkdd_settings_test COMMAND mkdd_settings_test)
# The controller ports: bindings to a GameCube controller, and scripted input.
add_executable(mkdd_input_test mkdd/tests/input_test.cpp mkdd/src/input.cpp mkdd/src/settings.cpp)
target_include_directories(mkdd_input_test PRIVATE mkdd/src host/include)
target_link_libraries(mkdd_input_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_input_test)
add_test(NAME mkdd_input_test COMMAND mkdd_input_test)

# The lifter's differential test: random encodings of every instruction in the
# slice, lifted at build time, run against an independent reference interpreter.
add_executable(qilin_difftest_gen recomp/tests/difftest/gen.cpp)
target_link_libraries(qilin_difftest_gen PRIVATE qilin_core qilin_warnings)
qilin_tool(qilin_difftest_gen)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    COMMAND qilin_difftest_gen ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp
    DEPENDS qilin_difftest_gen
    COMMENT "Generating the lifter's differential test cases")
add_executable(qilin_difftest
    recomp/tests/difftest/run.cpp
    recomp/tests/difftest/ref.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp)
target_include_directories(qilin_difftest PRIVATE recomp/tests/difftest)
target_link_libraries(qilin_difftest PRIVATE qilin_core qilin_rt qilin_warnings)
qilin_tool(qilin_difftest)
# Generated code: the lifter's output isn't held to our warning set (unused labels, locals).
if(MSVC)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/difftest_cases.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
add_test(NAME qilin_difftest COMMAND qilin_difftest)

# The runtime's floating-point helpers against exact rational arithmetic
# (recomp/tests/fp/fp_check.py): rounding, the NI flush, round25, the single
# multiply-add's one rounding, conversions, quantisation, and fres/frsqrte
# (with hardware results for frsqrte). Built with this compiler and run on this
# CPU, so each CI platform checks its own FPU's part in the results.
add_executable(qilin_fp_eval recomp/tests/fp/fp_eval.cpp)
target_link_libraries(qilin_fp_eval PRIVATE qilin_rt qilin_warnings)
qilin_tool(qilin_fp_eval)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME qilin_fp_check COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/recomp/tests/fp/fp_check.py
             $<TARGET_FILE:qilin_fp_eval> --cases 40000)
endif()

# The game's runtime: everything in mkdd but the code lifted from main.dol.
set(mkdd_runtime_sources mkdd/src/save_history.cpp mkdd/src/main.cpp mkdd/src/boot.cpp mkdd/src/os.cpp mkdd/src/dvd.cpp mkdd/src/vi.cpp mkdd/src/threads.cpp mkdd/src/sc.cpp mkdd/src/gpu.cpp mkdd/src/video.cpp mkdd/src/pad.cpp mkdd/src/card.cpp mkdd/src/exi.cpp mkdd/src/ar.cpp mkdd/src/later.cpp mkdd/src/hw.cpp mkdd/src/dsp.cpp mkdd/src/ax.cpp mkdd/src/recorder.cpp mkdd/src/texture.cpp mkdd/src/renderer.cpp mkdd/src/settings.cpp mkdd/src/restart.cpp mkdd/src/profile.cpp mkdd/src/menu.cpp mkdd/src/priority.cpp)
list(APPEND mkdd_runtime_sources mkdd/src/input.cpp)  # the controller ports (input.hpp)

# ---- The game: built only from the player's own main.dol ----
# cmake -DQILIN_GAME_DOL=<extracted disc>/sys/main.dol: qilin lift turns it
# into C++ at build time (into the build tree, never the source tree), with the
# library plan in data/R7PP01, and the mkdd executable is built from that.
set(QILIN_GAME_DOL "" CACHE FILEPATH "Mario Kart: Double Dash!! (PAL, Rev 1) main.dol: builds the mkdd executable")
if(QILIN_GAME_DOL)
    if(NOT QILIN_HOST)
        message(FATAL_ERROR "the game needs the host platform layer: configure with QILIN_HOST=ON")
    endif()
    set(QILIN_LIFT_FILES 32)
    set(gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(gen_sources ${gen}/table.cpp ${gen}/replaced.cpp)
    math(EXPR last "${QILIN_LIFT_FILES} - 1")
    foreach(i RANGE ${last})
        string(LENGTH "${i}" len)
        if(len EQUAL 1)
            set(i "00${i}")
        elseif(len EQUAL 2)
            set(i "0${i}")
        endif()
        list(APPEND gen_sources ${gen}/lifted_${i}.cpp)
    endforeach()
    add_custom_command(
        OUTPUT ${gen}/functions.hpp ${gen_sources}
        COMMAND qilin lift ${QILIN_GAME_DOL} -o ${gen} --plan ${CMAKE_CURRENT_SOURCE_DIR}/data/R7PP01/plan.tsv
                --files ${QILIN_LIFT_FILES}
        DEPENDS qilin ${QILIN_GAME_DOL} ${CMAKE_CURRENT_SOURCE_DIR}/data/R7PP01/plan.tsv
        COMMENT "Lifting the game's main.dol to C++")
    add_executable(mkdd ${mkdd_runtime_sources} ${gen_sources})
    target_include_directories(mkdd PRIVATE mkdd/src ${gen})
    target_link_libraries(mkdd PRIVATE qilin_rt qilin_dol qilin_host qilin_imgui SDL3::SDL3-static)
    if(WIN32)
        target_link_libraries(mkdd PRIVATE winmm)  # timeBeginPeriod (main.cpp)
    endif()
    target_compile_definitions(mkdd PRIVATE MKDD_VERSION="${PROJECT_VERSION}")
    target_compile_definitions(mkdd PRIVATE MKDD_DEVICES_STANDALONE)  # until the merge, as for mkdd_runtime_check below
    # Our own sources get the warning set; the lifter's output doesn't (unused labels and locals).
    set_source_files_properties(mkdd/src/main.cpp mkdd/src/boot.cpp mkdd/src/os.cpp mkdd/src/dvd.cpp mkdd/src/vi.cpp mkdd/src/threads.cpp mkdd/src/sc.cpp mkdd/src/gpu.cpp mkdd/src/video.cpp mkdd/src/pad.cpp mkdd/src/card.cpp mkdd/src/exi.cpp mkdd/src/ar.cpp mkdd/src/later.cpp mkdd/src/hw.cpp mkdd/src/dsp.cpp mkdd/src/ax.cpp mkdd/src/recorder.cpp mkdd/src/texture.cpp mkdd/src/settings.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W4;/permissive-,-Wall;-Wextra;-Wpedantic;-Wshadow>")
    # The renderer writes Vulkan structures {sType} with the rest zeroed, as the host layer does.
    set_source_files_properties(mkdd/src/renderer.cpp mkdd/src/menu.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W4;/permissive-,-Wall;-Wextra;-Wpedantic;-Wshadow;-Wno-missing-field-initializers>")
    if(MSVC)
        set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
        # Lifted code calls nest as deeply as the guest's: a large host stack.
        target_link_options(mkdd PRIVATE /STACK:16777216)
    else()
        if(QILIN_SANITIZE AND NOT QILIN_SANITIZE_LIFTED)
            set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "-w;-fno-sanitize=all;-g0")
        else()
            set_source_files_properties(${gen_sources} PROPERTIES COMPILE_OPTIONS "-w")
        endif()
    endif()
endif()

# ---- The player kit: everything mkdd needs but the code lifted from main.dol ----
# For the Windows download (tools/player-kit), as the APB recomp's: the game's
# runtime and every library it links (the host layer, SDL3, Dear ImGui, volk),
# merged into one archive, with the headers the lifted code includes, qilin,
# the library plan, and the flags and libraries to build the rest with
# (kit.ninja). A player's own PC lifts their own main.dol and compiles only
# that code, with the same compiler, against the kit. Needs a MinGW toolchain
# (tools/player-kit/mingw-cross.cmake) and SDL3 built statically (the fetched one).
option(QILIN_PLAYER_KIT "Build the player kit (tools/player-kit)" OFF)
if(QILIN_PLAYER_KIT)
    if(NOT QILIN_HOST OR NOT TARGET SDL3-static)
        message(FATAL_ERROR "the player kit needs the host layer with SDL3 built statically (QILIN_HOST=ON)")
    endif()
    add_library(mkdd_kit_main OBJECT ${mkdd_runtime_sources})
    target_include_directories(mkdd_kit_main PRIVATE mkdd/src)
    target_link_libraries(mkdd_kit_main PRIVATE qilin_rt qilin_dol qilin_host qilin_imgui SDL3::SDL3-static qilin_warnings)
    target_compile_definitions(mkdd_kit_main PRIVATE MKDD_VERSION="${PROJECT_VERSION}")
    set(kit_libraries qilin_rt qilin_dol qilin_host qilin_imgui qilin_volk SDL3-static)
    set(kit_archive "${CMAKE_BINARY_DIR}/libmkdd_kit.a")
    set(mri "CREATE ${kit_archive}\n")
    foreach(library IN LISTS kit_libraries)
        string(APPEND mri "ADDLIB $<TARGET_FILE:${library}>\n")
    endforeach()
    string(APPEND mri "ADDMOD $<JOIN:$<TARGET_OBJECTS:mkdd_kit_main>,\nADDMOD >\nSAVE\nEND\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/mkdd_kit.mri" CONTENT "${mri}")
    add_custom_command(OUTPUT "${kit_archive}"
        COMMAND "${CMAKE_COMMAND}" "-DAR=${CMAKE_AR}" "-DMRI=${CMAKE_BINARY_DIR}/mkdd_kit.mri" "-DOUT=${kit_archive}"
                -P "${CMAKE_SOURCE_DIR}/tools/player-kit/merge.cmake"
        DEPENDS mkdd_kit_main ${kit_libraries} $<TARGET_OBJECTS:mkdd_kit_main> "${CMAKE_BINARY_DIR}/mkdd_kit.mri"
        COMMENT "Merging the player kit's libraries" VERBATIM)
    add_custom_target(mkdd_kit ALL DEPENDS "${kit_archive}")
    # The system libraries SDL3 and the runtime need (MinGW names).
    set(kit_system_libraries -lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32
        -lsetupapi -lshell32 -ldinput8 -lwinspool -lcomdlg32)
    string(JOIN " " kit_system_text ${kit_system_libraries})
    # What the lifted code is compiled and linked with: kit.ninja, included by
    # the build.ninja the player's script writes ($kit is the kit's folder).
    # Lifted calls nest as deeply as the guest's: a 16 MB main stack, as MSVC's /STACK.
    file(WRITE "${CMAKE_BINARY_DIR}/kit.ninja"
        "# Written by CMake for the player kit (QILIN_PLAYER_KIT).\n"
        "cxxflags = -O2 -DNDEBUG -std=c++23 -w -ffp-contract=off -I$kit/include\n"
        "ldflags = -static -O2 -Wl,--stack,16777216\n"
        "libs = $kit/lib/libmkdd_kit.a ${kit_system_text}\n")
    # Linked with stand-ins for the lifted code: the kit is complete.
    add_executable(mkdd_kit_link_check tools/player-kit/link_check.cpp)
    target_link_libraries(mkdd_kit_link_check PRIVATE qilin_rt "${kit_archive}" ${kit_system_libraries})
    add_dependencies(mkdd_kit_link_check mkdd_kit)

    install(TARGETS qilin RUNTIME DESTINATION kit COMPONENT player)
    install(FILES "${kit_archive}" DESTINATION kit/lib COMPONENT player)
    install(FILES "${CMAKE_BINARY_DIR}/kit.ninja" data/R7PP01/plan.tsv DESTINATION kit COMPONENT player)
    file(GLOB kit_headers runtime/include/qilin/rt/*.hpp)
    install(FILES ${kit_headers} DESTINATION kit/include/qilin/rt COMPONENT player)
endif()

# Compile/link the complete runtime without distributing or requiring game data.
if(QILIN_HOST AND NOT QILIN_GAME_DOL AND NOT QILIN_PLAYER_KIT)
    add_executable(mkdd_runtime_check ${mkdd_runtime_sources} tools/player-kit/link_check.cpp)
    target_include_directories(mkdd_runtime_check PRIVATE mkdd/src)
    target_link_libraries(mkdd_runtime_check PRIVATE qilin_rt qilin_dol qilin_host qilin_imgui SDL3::SDL3-static qilin_warnings)
    target_compile_definitions(mkdd_runtime_check PRIVATE MKDD_VERSION="${PROJECT_VERSION}")
    # Until the host's input layer and the audio layer's ARAM are merged: pad.cpp and ar.cpp bring stand-ins.
    target_compile_definitions(mkdd_runtime_check PRIVATE MKDD_DEVICES_STANDALONE)
    if(WIN32)
        target_link_libraries(mkdd_runtime_check PRIVATE winmm)
    endif()
    if(NOT MSVC)
        set_source_files_properties(mkdd/src/renderer.cpp mkdd/src/menu.cpp PROPERTIES COMPILE_OPTIONS -Wno-missing-field-initializers)
    endif()
endif()

add_executable(mkdd_save_history_test mkdd/tests/save_history_test.cpp mkdd/src/save_history.cpp)
target_include_directories(mkdd_save_history_test PRIVATE mkdd/src)
target_link_libraries(mkdd_save_history_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_save_history_test)
add_test(NAME mkdd_save_history_test COMMAND mkdd_save_history_test)

# The devices' replacements over synthetic guest memory, without the game.
add_executable(mkdd_card_test mkdd/tests/card_test.cpp mkdd/src/card.cpp mkdd/src/later.cpp mkdd/src/save_history.cpp)
target_include_directories(mkdd_card_test PRIVATE mkdd/src host/include)
target_link_libraries(mkdd_card_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_card_test)
add_test(NAME mkdd_card_test COMMAND mkdd_card_test)

add_executable(mkdd_pad_test mkdd/tests/pad_test.cpp mkdd/src/pad.cpp mkdd/src/later.cpp)
target_include_directories(mkdd_pad_test PRIVATE mkdd/src)
target_link_libraries(mkdd_pad_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_pad_test)
add_test(NAME mkdd_pad_test COMMAND mkdd_pad_test)

# (With an ARAM block of its own: the real one is the audio layer's.)
add_executable(mkdd_ar_test mkdd/tests/ar_test.cpp mkdd/src/ar.cpp mkdd/src/exi.cpp mkdd/src/later.cpp)
target_include_directories(mkdd_ar_test PRIVATE mkdd/src)
target_compile_definitions(mkdd_ar_test PRIVATE MKDD_DEVICES_STANDALONE)
target_link_libraries(mkdd_ar_test PRIVATE qilin_rt qilin_warnings)
qilin_tool(mkdd_ar_test)
add_test(NAME mkdd_ar_test COMMAND mkdd_ar_test)

add_executable(qilin_analog_test host/tests/analog_test.cpp)
target_include_directories(qilin_analog_test PRIVATE host/include)
target_link_libraries(qilin_analog_test PRIVATE qilin_warnings)
qilin_tool(qilin_analog_test)
add_test(NAME qilin_analog_test COMMAND qilin_analog_test)
if(QILIN_HOST)
    add_executable(qilin_audio_callback_test host/tests/audio_callback_test.cpp)
    target_link_libraries(qilin_audio_callback_test PRIVATE qilin_host SDL3::SDL3-static qilin_warnings)
    qilin_tool(qilin_audio_callback_test)
    add_test(NAME qilin_audio_callback_test COMMAND qilin_audio_callback_test)
    set_tests_properties(qilin_audio_callback_test PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy;SDL_AUDIO_DEVICE_SAMPLE_FRAMES=128" TIMEOUT 25)
endif()

if(Python3_FOUND)
    add_test(NAME mkdd_timing_check_test COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/timing_check_test.py)
endif()
if(Python3_FOUND AND UNIX)
    add_test(NAME mkdd_launcher_test COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/launcher_test.py)
endif()
find_program(QILIN_POWERSHELL NAMES pwsh powershell)
if(QILIN_POWERSHELL)
    add_test(NAME mkdd_powershell_syntax_test COMMAND ${QILIN_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
             -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/tests/powershell_syntax_test.ps1)
endif()
