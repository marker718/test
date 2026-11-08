# Host build of the doa component + synthetic test harness.
#
#   cmake -S fw/components/doa/test_host -B build/doa-host -G Ninja
#   cmake --build build/doa-host && ctest --test-dir build/doa-host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(doa_host_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(DOA_SANITIZE "Build with ASan + UBSan" ON)
if(DOA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. doa)

add_executable(doa_test test_doa.c synth.c)
# test_doa.c pokes at the private FFT directly.
target_include_directories(doa_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../src)
target_compile_definitions(doa_test PRIVATE _DEFAULT_SOURCE) # M_PI
target_compile_options(doa_test PRIVATE -Wall -Wextra -Wshadow -Werror)
target_link_libraries(doa_test PRIVATE doa m)

enable_testing()
add_test(NAME doa COMMAND doa_test)
