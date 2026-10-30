# Third-party dependencies: spdlog, inipp and imgui (git submodules), safetyhook (vendored).

set(EXTERNAL_DIR ${PROJECT_SOURCE_DIR}/external)

foreach(required_file
        ${EXTERNAL_DIR}/spdlog/include/spdlog/spdlog.h
        ${EXTERNAL_DIR}/inipp/inipp/inipp.h
        ${EXTERNAL_DIR}/imgui/imgui.h)
    if(NOT EXISTS ${required_file})
        message(FATAL_ERROR
            "Missing ${required_file}\n"
            "The git submodules are not initialized. Run: make submodules\n"
            "(or: git submodule update --init --recursive)")
    endif()
endforeach()

# spdlog: header-only
add_library(spdlog_headers INTERFACE)
target_include_directories(spdlog_headers SYSTEM INTERFACE ${EXTERNAL_DIR}/spdlog/include)
add_library(deps::spdlog ALIAS spdlog_headers)

# inipp: header-only
add_library(inipp INTERFACE)
target_include_directories(inipp SYSTEM INTERFACE ${EXTERNAL_DIR}/inipp)
add_library(deps::inipp ALIAS inipp)

# safetyhook: amalgamated sources (includes Zydis)
add_library(safetyhook STATIC
    ${EXTERNAL_DIR}/safetyhook/safetyhook.cpp
    ${EXTERNAL_DIR}/safetyhook/Zydis.c)
target_include_directories(safetyhook SYSTEM PUBLIC ${EXTERNAL_DIR}/safetyhook)
if(NOT MSVC)
    target_compile_options(safetyhook PRIVATE -w) # third-party code, keep the build output quiet
endif()
add_library(deps::safetyhook ALIAS safetyhook)

# imgui: core + Win32/DX12 backends for the in-game overlay
set(IMGUI_DIR ${EXTERNAL_DIR}/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# d3d12/dxgi must not be imported: a static dxgi.dll import loads the system dxgi before the game does,
# which breaks dxgi.dll proxies such as OptiScaler. The backend's CreateDXGIFactory1 call is redirected to
# a runtime-resolved version in src/overlay.cpp. d3dcompiler compiles the overlay shaders.
target_compile_definitions(imgui PUBLIC CreateDXGIFactory1=Overlay_CreateDXGIFactory1)
if(MINGW)
    target_link_libraries(imgui PUBLIC d3dcompiler_47 dwmapi gdi32)
else()
    target_link_libraries(imgui PUBLIC d3dcompiler dwmapi gdi32)
endif()
if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
endif()
add_library(deps::imgui ALIAS imgui)
