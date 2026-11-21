cmake_minimum_required(VERSION 3.25)

# Static CRT (/MT) so the game process needs no redistributable.
# Must come before project() and all targets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ACPReloaded VERSION 1.0.0 LANGUAGES C CXX)
set(ACP_VERSION_LABEL "${PROJECT_VERSION}-beta")  # shown in the panel and the log

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "ACP Reloaded builds only with MSVC for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACP_DEV_BUILD "Entwicklerbuild (Hard-Unload erlaubt)" OFF)
set(ACP_GAME_DIR "" CACHE PATH "Game folder for the deploy target")

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

# --- acp_core: parameter table and INI document (pure C++, no Win32) ---
add_library(acp_core STATIC
    src/core/param_types.h src/core/param_table.h src/core/param_table.cpp
    src/core/param_ids.inc src/core/param_table_data.inc
    src/core/ini_document.h src/core/ini_document.cpp)
target_include_directories(acp_core PUBLIC src/core)
acp_set_warnings(acp_core)

# --- acp_platform: paths, atomic writes, log (Win32) ---
add_library(acp_platform STATIC src/platform/platform.h src/platform/platform.cpp)
target_include_directories(acp_platform PUBLIC src/platform)
target_link_libraries(acp_platform PUBLIC shell32 ole32 advapi32)
acp_set_warnings(acp_platform)

# --- acp_sync: I/O thread, deltas, snapshots, presets ---
add_library(acp_sync STATIC src/sync/sync_service.h src/sync/sync_service.cpp)
target_include_directories(acp_sync PUBLIC src/sync)
target_link_libraries(acp_sync PUBLIC acp_core acp_platform)
acp_set_warnings(acp_sync)

# --- acp_input: PadFilter (pure) ---
add_library(acp_input STATIC src/input/pad_filter.h src/input/pad_filter.cpp)
target_include_directories(acp_input PUBLIC src/input)
acp_set_warnings(acp_input)

# --- acp_ui: theme, widgets, panel, pad input feed (ImGui) ---
add_library(acp_ui STATIC
    src/ui/theme.h src/ui/theme.cpp
    src/ui/widgets.h src/ui/widgets.cpp
    src/ui/icons.h src/ui/icons.cpp
    src/ui/pad_feed.h src/ui/pad_feed.cpp
    src/ui/panel.h src/ui/panel.cpp
    src/ui/portraits.h src/ui/portraits.cpp)
# Heading font Cinzel (SIL OFL 1.1, third_party/fonts) embedded - no files in the game folder
set(ACP_FONT_INC ${CMAKE_CURRENT_BINARY_DIR}/generated/font_cinzel.inc)
add_custom_command(
    OUTPUT ${ACP_FONT_INC}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Cinzel.ttf -DOUT=${ACP_FONT_INC}
            -DNAME=kCinzelTtf -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/Cinzel.ttf ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding Cinzel font")
target_sources(acp_ui PRIVATE ${ACP_FONT_INC})
target_include_directories(acp_ui PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(acp_ui PUBLIC src/ui)
target_compile_definitions(acp_ui PUBLIC ACP_VERSION_LABEL="${ACP_VERSION_LABEL}")
target_link_libraries(acp_ui PUBLIC acp_imgui acp_sync acp_input d3d11 windowscodecs ole32)
# Character portraits: RCDATA resources (assets/portraits/portraits.rc), embedded in each module (DLL or editor)
target_include_directories(acp_ui PRIVATE assets/portraits)
set(ACP_PORTRAITS_RC ${CMAKE_CURRENT_SOURCE_DIR}/assets/portraits/portraits.rc)
acp_set_warnings(acp_ui)

# --- ui_sandbox: standalone editor (Win32 + DX11) ---
add_executable(ui_sandbox WIN32 tools/ui_sandbox/main.cpp ${ACP_PORTRAITS_RC})
target_link_libraries(ui_sandbox PRIVATE acp_ui xinput shell32)
acp_set_warnings(ui_sandbox)
set_target_properties(ui_sandbox PROPERTIES OUTPUT_NAME "ACPReloaded_Editor")

# --- ACPReloaded.dll: Overlay ---
add_library(ACPReloaded SHARED
    src/hook/hooks.h
    src/hook/overlay.h src/hook/overlay.cpp
    src/hook/vtable_probe.cpp
    src/hook/dx11_hooks.cpp
    src/hook/wndproc_hook.cpp
    src/hook/xinput_hook.cpp
    src/hook/uplay_hook.cpp
    src/hook/postfx.h src/hook/postfx.cpp
    src/hook/frame_pacer.h src/hook/frame_pacer.cpp
    src/game/game_bridge.h src/game/game_bridge.cpp src/game/game_table_16965472.inc
    src/hook/lifecycle.cpp
    src/dllmain.cpp
    ${ACP_PORTRAITS_RC})
target_include_directories(ACPReloaded PRIVATE src/hook src/game)
target_link_libraries(ACPReloaded PRIVATE acp_ui acp_minhook d3d11 dxgi)
if(ACP_DEV_BUILD)
    target_compile_definitions(ACPReloaded PRIVATE ACP_DEV_BUILD=1)
endif()
acp_set_warnings(ACPReloaded)

# --- dinput8.dll: optional proxy loader (own output folder) ---
add_library(dinput8_proxy SHARED src/proxy/dinput8_proxy.cpp src/proxy/dinput8.def)
set_target_properties(dinput8_proxy PROPERTIES
    OUTPUT_NAME dinput8
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/proxy)
acp_set_warnings(dinput8_proxy)

# --- deploy: copies proxy + overlay into the game folder (run only explicitly) ---
if(ACP_GAME_DIR)
    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:dinput8_proxy> "${ACP_GAME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ACPReloaded> "${ACP_GAME_DIR}"
        DEPENDS dinput8_proxy ACPReloaded
        COMMENT "Copying dinput8.dll and ACPReloaded.dll to ${ACP_GAME_DIR}")
endif()

# --- Tests ---
enable_testing()
add_executable(acp_tests tests/acp_tests.cpp tests/sync_tests.cpp tests/pad_filter_tests.cpp tests/proxy_tests.cpp
    tests/pacing_tests.cpp tests/test_main.h src/hook/frame_pacer.cpp)
target_include_directories(acp_tests PRIVATE src/hook)
target_link_libraries(acp_tests PRIVATE acp_core acp_sync acp_input dxguid)
add_dependencies(acp_tests dinput8_proxy)
target_compile_definitions(acp_tests PRIVATE
    ACP_REFERENCE_INI="${CMAKE_CURRENT_SOURCE_DIR}/copys/ACP.ini"
    ACP_PARAM_CSV="${CMAKE_CURRENT_SOURCE_DIR}/docs/param_map.csv"
    ACP_PROXY_DLL=L"$<TARGET_FILE:dinput8_proxy>")
acp_set_warnings(acp_tests)
add_test(NAME acp_tests COMMAND acp_tests)
