cmake_minimum_required(VERSION 3.0.2)

option(SLZ_TESTS "Enable tests" ON)

project(slz C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_definitions(PRECOMPUTE_TABLES)
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/O2 /W4 -Wno-deprecated-declarations)
else()
    add_compile_options(-O3 -Wall -fomit-frame-pointer -g)
endif()

add_library(slz src/slz.c src/slz.h src/tables.h)

add_executable(zenc src/zenc.c)
target_link_libraries(zenc slz)

add_executable(zdec src/zdec.c)
target_link_libraries(zdec slz)

if(SLZ_TESTS)
    enable_testing()

    file(GLOB TEST_FILES tests/*)
    foreach (TEST_FILE IN LISTS TEST_FILES)
        get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)

        set(ZENC_COMMAND $<TARGET_FILE:zenc> -9 -c)
        message(STATUS ${CMAKE_CURRENT_SOURCE_DIR}/temp/${TEST_NAME}.zenc)

        add_test(NAME zenc-${TEST_NAME}
            COMMAND ${CMAKE_COMMAND}
            "-DCOMMAND=${ZENC_COMMAND}"
            -DCOMPRESS_TARGET=$<TARGET_FILE:zenc>
            "-DCOMPRESS_ARGS=-9;-c;-G"
            -DDECOMPRESS_TARGET=$<TARGET_FILE:zdec>
            "-DDECOMPRESS_ARGS="
            -DINPUT=${TEST_FILE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test-compress.cmake)

        #message(STATUS $<TARGET_FILE:zdec> < ${TEST_FILE}.zenc > ${TEST_FILE}.zdec)
        #add_test(NAME zdec-${TEST_NAME}
         #   COMMAND $<TARGET_FILE:zdec> < ${TEST_FILE}.zenc > ${TEST_FILE}.zdec)
    endforeach()
endif()
