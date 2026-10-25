# FRAME_VERSION: what the apps show as their version, the same name the release gets (.github/workflows/build.yml):
# "v" + VERSION (major.minor) + "." + the commits since VERSION last changed, so the commit that bumps VERSION is
# v<major.minor>.0. $FRAME_VERSION overrides it. Without the whole history (a shallow checkout, a copy that isn't a
# git repository) the count is unknown: "v0.2 (local)"; VERSION edited but not committed yet counts from 0.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" FRAME_VERSION_BASE)
string(STRIP "${FRAME_VERSION_BASE}" FRAME_VERSION_BASE)
set(FRAME_VERSION "$ENV{FRAME_VERSION}")
if(NOT FRAME_VERSION)
    set(FRAME_VERSION "v${FRAME_VERSION_BASE} (local)")
    set(FRAME_SRC "${CMAKE_CURRENT_LIST_DIR}/..")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --is-shallow-repository WORKING_DIRECTORY "${FRAME_SRC}"
                        OUTPUT_VARIABLE FRAME_SHALLOW RESULT_VARIABLE FRAME_GIT_RC OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(FRAME_GIT_RC EQUAL 0 AND FRAME_SHALLOW STREQUAL "false")
            execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD -- VERSION WORKING_DIRECTORY "${FRAME_SRC}"
                            RESULT_VARIABLE FRAME_VERSION_EDITED ERROR_QUIET)
            execute_process(COMMAND "${GIT_EXECUTABLE}" log -1 --format=%H -- VERSION WORKING_DIRECTORY "${FRAME_SRC}"
                            OUTPUT_VARIABLE FRAME_BUMP RESULT_VARIABLE FRAME_GIT_RC OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(FRAME_VERSION_EDITED EQUAL 1)
                set(FRAME_VERSION "v${FRAME_VERSION_BASE}.0 (local)")
            elseif(FRAME_GIT_RC EQUAL 0 AND FRAME_BUMP)
                execute_process(COMMAND "${GIT_EXECUTABLE}" rev-list --count "${FRAME_BUMP}..HEAD" WORKING_DIRECTORY "${FRAME_SRC}"
                                OUTPUT_VARIABLE FRAME_COMMITS RESULT_VARIABLE FRAME_GIT_RC OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
                if(FRAME_GIT_RC EQUAL 0 AND FRAME_COMMITS MATCHES "^[0-9]+$")
                    set(FRAME_VERSION "v${FRAME_VERSION_BASE}.${FRAME_COMMITS}")
                endif()
            endif()
        endif()
    endif()
endif()
message(STATUS "Version: ${FRAME_VERSION}")
