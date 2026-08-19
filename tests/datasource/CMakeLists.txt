cmake_minimum_required(VERSION 3.16)
project(deskos_datasource_tests C)

# Task #2 in ROADMAP.md: datasource_resolve/datasource_render are pure functions
# over cJSON, the one place in this project where a host-compiled unit test
# is cheap and meaningful. This is a plain CMake project, deliberately not an
# ESP-IDF component build — it needs a host compiler (see CLAUDE.md, no
# gcc/cl/clang shipped with the ESP-IDF Windows toolchain) and cJSON's own
# source, borrowed from the ESP-IDF install rather than vendored again.

set(WIDGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/widget")

if(NOT IDF_PATH)
    if(DEFINED ENV{IDF_PATH})
        # export.ps1 sets IDF_PATH with backslashes; CMake's quoted-string
        # escape processing chokes on some of them (e.g. the "\e" in "\esp").
        file(TO_CMAKE_PATH "$ENV{IDF_PATH}" IDF_PATH)
    else()
        set(IDF_PATH "C:/esp/esp-idf")
    endif()
endif()
set(CJSON_DIR "${IDF_PATH}/components/json/cJSON")

if(NOT EXISTS "${CJSON_DIR}/cJSON.c")
    message(FATAL_ERROR
        "cJSON not found under ${CJSON_DIR}. Activate the ESP-IDF environment "
        "first (export.ps1 sets IDF_PATH), or pass -DIDF_PATH=...")
endif()

add_executable(test_datasource
    test_datasource.c
    "${WIDGET_DIR}/datasource.c"
    "${CJSON_DIR}/cJSON.c"
)
target_include_directories(test_datasource PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/shim"
    "${WIDGET_DIR}/include"
    "${CJSON_DIR}"
)

enable_testing()
add_test(NAME datasource COMMAND test_datasource)
