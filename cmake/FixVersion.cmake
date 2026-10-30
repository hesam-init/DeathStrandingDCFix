# Fix version: shown in the log and the in-game menu (sFixVersion) and used in the release zip name.
# -DFIX_VERSION=<version> sets it (create_release.ps1 / the release workflow pass the git tag).
# Otherwise it comes from `git describe --tags`: "1.2.3" on a tag, "1.2.3-4-gabc1234" after it.
# A leading "v" is dropped; without git (or outside a checkout) it is "dev".
# Result: FIX_VERSION_STRING

set(FIX_VERSION "" CACHE STRING "Fix version (empty: from git describe)")
set(FIX_VERSION_STRING "${FIX_VERSION}")

if(NOT FIX_VERSION_STRING)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE FIX_VERSION_STRING
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

        # Re-run configure when HEAD moves (commit, checkout) so the version follows it.
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --symbolic-full-name HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE git_head_ref
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        foreach(git_ref HEAD ${git_head_ref})
            execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${git_ref}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE git_ref_file
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(git_ref_file)
                cmake_path(ABSOLUTE_PATH git_ref_file BASE_DIRECTORY ${CMAKE_SOURCE_DIR})
                if(EXISTS ${git_ref_file})
                    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${git_ref_file})
                endif()
            endif()
        endforeach()
    endif()
endif()

string(REGEX REPLACE "^v" "" FIX_VERSION_STRING "${FIX_VERSION_STRING}")
if(NOT FIX_VERSION_STRING)
    set(FIX_VERSION_STRING dev)
endif()
message(STATUS "Fix version: ${FIX_VERSION_STRING}")
