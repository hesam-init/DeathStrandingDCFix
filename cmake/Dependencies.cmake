# Third-party dependencies, downloaded at configure time from their release tags.
# To upgrade a library, change its version below and rebuild.
# Offline / local patching: -DFETCHCONTENT_SOURCE_DIR_<NAME>=<path> (e.g. FETCHCONTENT_SOURCE_DIR_IMGUI)
# points a dependency at a local checkout instead of downloading it.

set(IMGUI_VERSION      1.93.0)
set(SPDLOG_VERSION     1.17.0)
set(INIPP_VERSION      1.0.13)
set(SAFETYHOOK_VERSION 0.7.0)
set(ZYDIS_VERSION      4.1.0)  # the version safetyhook pins (its own CMake would git clone it)

# safetyhook and Zydis come from release assets, which aren't immutable like tag archives:
# update the hash together with the version.
set(SAFETYHOOK_SHA256 0f34f8a8561bd9b2f2e6cf03bd68f69a69b9437a83d38d435c0cbf0828c75c64)
set(ZYDIS_SHA256      6ce26e5bf812aa9384af4a514fe81afa53f3fd00d579ba9d80a710e2218f48ff)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF) # show download/extract output while configuring

# SOURCE_SUBDIR points at a directory that doesn't exist: download only, targets are defined below.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v${IMGUI_VERSION}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _download_only)

FetchContent_Declare(spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v${SPDLOG_VERSION}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _download_only)

FetchContent_Declare(inipp
    URL https://github.com/mcmtroffaes/inipp/archive/refs/tags/${INIPP_VERSION}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _download_only)

# Amalgamated single-file releases (safetyhook.hpp/.cpp, Zydis.h/.c): no git clone of Zydis and Zycore.
FetchContent_Declare(safetyhook
    URL https://github.com/cursey/safetyhook/releases/download/v${SAFETYHOOK_VERSION}/safetyhook-amalgamated.zip
    URL_HASH SHA256=${SAFETYHOOK_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _download_only)

FetchContent_Declare(zydis
    URL https://github.com/zyantific/zydis/releases/download/v${ZYDIS_VERSION}/zydis-amalgamated.zip
    URL_HASH SHA256=${ZYDIS_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _download_only)

FetchContent_MakeAvailable(imgui spdlog inipp safetyhook zydis)

# spdlog: header-only
add_library(spdlog_headers INTERFACE)
target_include_directories(spdlog_headers SYSTEM INTERFACE ${spdlog_SOURCE_DIR}/include)
add_library(deps::spdlog ALIAS spdlog_headers)

# inipp: header-only
add_library(inipp INTERFACE)
target_include_directories(inipp SYSTEM INTERFACE ${inipp_SOURCE_DIR})
add_library(deps::inipp ALIAS inipp)

# Zydis: static (otherwise Zydis.h marks its API dllimport)
add_library(zydis STATIC ${zydis_SOURCE_DIR}/Zydis.c)
target_include_directories(zydis SYSTEM PUBLIC ${zydis_SOURCE_DIR})
target_compile_definitions(zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)

# safetyhook
add_library(safetyhook STATIC ${safetyhook_SOURCE_DIR}/safetyhook.cpp)
target_include_directories(safetyhook SYSTEM PUBLIC ${safetyhook_SOURCE_DIR})
target_compile_features(safetyhook PUBLIC cxx_std_23)
target_link_libraries(safetyhook PUBLIC zydis)
if(NOT MSVC)
    foreach(target safetyhook zydis)
        target_compile_options(${target} PRIVATE -w) # third-party code, keep the build output quiet
    endforeach()
endif()
add_library(deps::safetyhook ALIAS safetyhook)

# imgui: core + Win32/DX12 backends for the in-game overlay
set(IMGUI_DIR ${imgui_SOURCE_DIR})
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
