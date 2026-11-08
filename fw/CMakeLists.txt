cmake_minimum_required(VERSION 3.16)

# Monorepo-aware version string. IDF's default `git describe` would grab the
# nearest tag of ANY kind (including hw-* tags), so pin it to fw-v* tags.
# Evaluated at configure time: run `idf.py reconfigure` after tagging.
execute_process(
    COMMAND git describe --tags --match "fw-v*" --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE FW_GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(FW_GIT_DESC)
    set(PROJECT_VER "${FW_GIT_DESC}")
else()
    set(PROJECT_VER "0.0.0-nogit")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)
