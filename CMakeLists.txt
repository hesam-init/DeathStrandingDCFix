cmake_minimum_required(VERSION 3.28) # FetchContent SYSTEM/EXCLUDE_FROM_ALL; safetyhook requires 3.28

project(DeathStrandingDCFix LANGUAGES C CXX)

include(cmake/FixVersion.cmake) # FIX_VERSION_STRING: -DFIX_VERSION or the git tag

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Optimize for size by default
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # no C++20 modules here; skip the dependency scan 3.28 enables
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release folder: winhttp.dll (the fix, loaded by the game as a proxy) and its .ini end up together here.
set(RELEASE_DIR ${CMAKE_BINARY_DIR}/bin)

if(MINGW)
    # LTO is intentionally not enabled: mingw-w64-gcc 16.2 hits an internal
    # compiler error (binds_to_current_def_p) with -flto on this codebase.
    add_compile_options(
        -finput-charset=UTF-8
        $<$<NOT:$<CONFIG:Debug>>:-ffunction-sections>
        $<$<NOT:$<CONFIG:Debug>>:-fdata-sections>)
elseif(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
endif()

include(cmake/Dependencies.cmake)

# Built as winhttp.dll: ds.exe imports WINHTTP.dll, so the game loads the fix directly and every winhttp
# export is forwarded to the system winhttp.dll (src/proxy). No ASI loader needed.
add_library(${PROJECT_NAME} SHARED
    src/dllmain.cpp
    src/overlay.cpp
    src/proxy/winhttp.cpp
    src/proxy/winhttp.def)
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "winhttp"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY ${RELEASE_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    FIX_VERSION="${FIX_VERSION_STRING}"
    $<$<CONFIG:Debug>:_DEBUG>)
target_link_libraries(${PROJECT_NAME} PRIVATE deps::spdlog deps::inipp deps::safetyhook deps::imgui user32)

if(MINGW)
    # Fully static so the game folder needs no libstdc++/libgcc/winpthread DLLs.
    target_link_options(${PROJECT_NAME} PRIVATE
        -static -static-libgcc -static-libstdc++
        $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
        $<$<NOT:$<CONFIG:Debug>>:-s>)
    # GCC < 15 (e.g. Debian trixie's 14.2) emits C++23's inline std::type_info::operator== into our
    # objects while the static libstdc++ also defines it (GCC PR 110572), so -static fails to link
    # with "multiple definition". Both copies are the same function; let the linker keep one.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        target_link_options(${PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)
    endif()
endif()

# Copy the default ini next to winhttp.dll (re-copied on reconfigure).
configure_file(${PROJECT_NAME}.ini ${RELEASE_DIR}/${PROJECT_NAME}.ini COPYONLY)

# Install into a game folder: cmake --install <build> --component game --prefix <game dir>
# (winhttp.dll replaces the Ultimate ASI Loader older versions shipped under the same name)
install(FILES $<TARGET_FILE:${PROJECT_NAME}> DESTINATION . COMPONENT game)
install(CODE "
    set(old_asi \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.asi\")
    if(EXISTS \"\${old_asi}\")
        message(STATUS \"Removing old: \${old_asi}\")
        file(REMOVE \"\${old_asi}\")
    endif()
    set(ini \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.ini\")
    if(EXISTS \"\${ini}\")
        message(STATUS \"Keeping existing: \${ini}\")
    else()
        message(STATUS \"Installing: \${ini}\")
        file(COPY_FILE \"${RELEASE_DIR}/${PROJECT_NAME}.ini\" \"\${ini}\")
    endif()
" COMPONENT game)

# Release zip only: marker file telling users where to extract it.
file(TOUCH ${CMAKE_BINARY_DIR}/EXTRACT_TO_GAME_FOLDER)
install(FILES ${CMAKE_BINARY_DIR}/EXTRACT_TO_GAME_FOLDER DESTINATION . COMPONENT package)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME ${PROJECT_NAME})
set(CPACK_PACKAGE_FILE_NAME ${PROJECT_NAME}_${FIX_VERSION_STRING})
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
