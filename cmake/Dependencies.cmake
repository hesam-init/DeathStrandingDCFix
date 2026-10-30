# Third-party dependencies: spdlog and inipp (git submodules), safetyhook (vendored).

set(EXTERNAL_DIR ${PROJECT_SOURCE_DIR}/external)

foreach(required_file
        ${EXTERNAL_DIR}/spdlog/include/spdlog/spdlog.h
        ${EXTERNAL_DIR}/inipp/inipp/inipp.h)
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
