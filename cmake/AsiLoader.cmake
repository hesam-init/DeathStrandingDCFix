# Ultimate ASI Loader (x64), shipped as winhttp.dll so the game loads the .asi.
# Downloaded once into ASI_LOADER_CACHE_DIR (shared by all build dirs, kept by 'make clean').
# Sets ASI_LOADER_PATH.

set(ASI_LOADER_URL
    "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest/download/Ultimate-ASI-Loader_x64.zip"
    CACHE STRING "Ultimate ASI Loader x64 release zip")
set(ASI_LOADER_CACHE_DIR "${PROJECT_SOURCE_DIR}/build/asi-loader"
    CACHE PATH "Where the downloaded ASI loader is cached")
set(ASI_LOADER_DLL ""
    CACHE FILEPATH "Use this local ASI loader dll instead of downloading one")
set(ASI_LOADER_PROXY_NAME "winhttp.dll")

if(ASI_LOADER_DLL)
    if(NOT EXISTS "${ASI_LOADER_DLL}")
        message(FATAL_ERROR "ASI_LOADER_DLL does not exist: ${ASI_LOADER_DLL}")
    endif()
    set(ASI_LOADER_PATH "${ASI_LOADER_DLL}")
    return()
endif()

set(ASI_LOADER_PATH "${ASI_LOADER_CACHE_DIR}/${ASI_LOADER_PROXY_NAME}")

if(NOT EXISTS "${ASI_LOADER_PATH}")
    set(zip "${ASI_LOADER_CACHE_DIR}/asi-loader.zip")
    message(STATUS "Downloading Ultimate ASI Loader: ${ASI_LOADER_URL}")
    file(DOWNLOAD "${ASI_LOADER_URL}" "${zip}" STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        list(GET download_status 1 download_error)
        file(REMOVE "${zip}")
        message(FATAL_ERROR
            "Failed to download the ASI loader: ${download_error}\n"
            "Download Ultimate-ASI-Loader_x64.zip manually, extract dinput8.dll and either\n"
            "  - save it as ${ASI_LOADER_PATH}, or\n"
            "  - configure with -DASI_LOADER_DLL=/path/to/dinput8.dll")
    endif()

    file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${ASI_LOADER_CACHE_DIR}" PATTERNS dinput8.dll)
    file(REMOVE "${zip}")
    if(NOT EXISTS "${ASI_LOADER_CACHE_DIR}/dinput8.dll")
        message(FATAL_ERROR "dinput8.dll not found in ${ASI_LOADER_URL}")
    endif()
    file(RENAME "${ASI_LOADER_CACHE_DIR}/dinput8.dll" "${ASI_LOADER_PATH}")
endif()

message(STATUS "ASI loader: ${ASI_LOADER_PATH}")
