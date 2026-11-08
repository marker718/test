# Dual-mode: ESP-IDF component on target, plain static library on the host
# (pulled in by test_host/CMakeLists.txt).

set(DOA_SRCS src/doa.c src/doa_fft.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${DOA_SRCS}
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS src
    )
    # DSP hot path: optimise even in debug builds, and flag any accidental
    # float->double promotion (soft-float on the S3).
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wdouble-promotion)
    return()
endif()

add_library(doa STATIC ${DOA_SRCS})
target_include_directories(doa PUBLIC include PRIVATE src)
target_compile_options(doa PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wdouble-promotion -Werror)
target_link_libraries(doa PUBLIC m)
