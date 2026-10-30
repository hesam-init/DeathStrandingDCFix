cmake_minimum_required(VERSION 3.24)

project(DeathStrandingDCFix LANGUAGES C CXX)

set(FIX_VERSION "dev" CACHE STRING "Version used in the release zip name")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # Matches xmake's set_optimize("smallest")
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release folder: the .asi, its .ini and the ASI loader end up together here.
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
include(cmake/AsiLoader.cmake)

add_library(${PROJECT_NAME} SHARED src/dllmain.cpp)
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY ${RELEASE_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
target_link_libraries(${PROJECT_NAME} PRIVATE deps::spdlog deps::inipp deps::safetyhook user32)

if(MINGW)
    # Fully static so the game folder needs no libstdc++/libgcc/winpthread DLLs.
    target_link_options(${PROJECT_NAME} PRIVATE
        -static -static-libgcc -static-libstdc++
        $<$<NOT:$<CONFIG:Debug>>:-Wl,--gc-sections>
        $<$<NOT:$<CONFIG:Debug>>:-s>)
endif()

# Copy the default ini and the ASI loader next to the .asi (re-copied on reconfigure).
configure_file(${PROJECT_NAME}.ini ${RELEASE_DIR}/${PROJECT_NAME}.ini COPYONLY)
configure_file(${ASI_LOADER_PATH} ${RELEASE_DIR}/${ASI_LOADER_PROXY_NAME} COPYONLY)

# Install into a game folder: cmake --install <build> --component game --prefix <game dir>
install(FILES $<TARGET_FILE:${PROJECT_NAME}> ${RELEASE_DIR}/${ASI_LOADER_PROXY_NAME}
    DESTINATION . COMPONENT game)
install(CODE "
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
set(CPACK_PACKAGE_FILE_NAME ${PROJECT_NAME}_${FIX_VERSION})
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
