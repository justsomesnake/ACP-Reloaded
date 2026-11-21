# Third-party dependencies, pinned to commits. Own targets instead of upstream CMake,
# so /MT applies everywhere and no install rules come along.
include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG c1aaac65e896a45f81f47a4d68f9959e0f86c859  # v1.93.0
    SOURCE_SUBDIR _none)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537  # v1.3.4
    SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(imgui minhook)

add_library(acp_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(acp_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                                            ${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_config)
target_compile_definitions(acp_imgui PUBLIC IMGUI_USER_CONFIG="acp_imconfig.h")
target_compile_options(acp_imgui PRIVATE /utf-8 /W3)
target_link_libraries(acp_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)

add_library(acp_minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(acp_minhook PUBLIC ${minhook_SOURCE_DIR}/include)
target_compile_options(acp_minhook PRIVATE /W3)
