cmake_minimum_required(VERSION 3.20)
project(idx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Posting codec selection: VarByte (default) | Raw32. Used by P2 to build two
# parallel indexes for the compression-ratio benchmark.
set(IDX_CODEC "VarByte" CACHE STRING "Posting codec: VarByte | Raw32")
set_property(CACHE IDX_CODEC PROPERTY STRINGS VarByte Raw32)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fno-omit-frame-pointer)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_compile_options(-mcpu=apple-m1)
    elseif(NOT APPLE)
        add_compile_options(-march=native)
    endif()
endif()

option(IDX_ASAN "Enable AddressSanitizer + UBSan" OFF)
if(IDX_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Build the index using the legacy (default-allocator) PartialIndex storage,
# rather than the SlabArena + std::pmr path. Used as the "before" arm of the
# memory benchmark.
option(IDX_BUILDER_BASELINE "Use default allocator instead of SlabArena in builder" OFF)

find_package(Threads REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

# Common codec macro: every target that touches index bytes must agree on
# encoding. Builder and search_engine both pick this up.
set(IDX_CODEC_DEFINE "IDX_CODEC_${IDX_CODEC}")

# ----------------------------------------------------------------------------
# Index builder (library + thin CLI).
# ----------------------------------------------------------------------------
add_library(builder STATIC src/builder.cpp)
target_compile_definitions(builder PUBLIC ${IDX_CODEC_DEFINE})
if(IDX_BUILDER_BASELINE)
    target_compile_definitions(builder PRIVATE IDX_BUILDER_BASELINE)
endif()

add_executable(build_index src/build_index.cpp)
target_link_libraries(build_index PRIVATE builder)

# ----------------------------------------------------------------------------
# Query path (PIMPL SearchEngine + interactive / JSONL CLI).
# ----------------------------------------------------------------------------
add_library(search_engine STATIC
    src/search_engine.cpp
    src/inverted_list.cpp)
target_compile_definitions(search_engine PUBLIC ${IDX_CODEC_DEFINE})
target_link_libraries(search_engine PUBLIC Threads::Threads)

add_executable(search_cli src/search_cli.cpp)
target_link_libraries(search_cli PRIVATE search_engine)

# ----------------------------------------------------------------------------
# Phase skeletons (filled in during the corresponding phase).
# ----------------------------------------------------------------------------
add_executable(bench_compression bench/bench_compression.cpp)
add_executable(bench_index_size  bench/bench_index_size.cpp)

add_executable(bench_latency   bench/bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE search_engine)

add_executable(trec_run_writer eval/trec_run_writer.cpp)
target_link_libraries(trec_run_writer PRIVATE search_engine)

# ----------------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------------
enable_testing()

add_executable(test_varbyte       tests/test_varbyte.cpp)
add_executable(test_raw32         tests/test_raw32.cpp)
add_executable(test_codec         tests/test_codec.cpp)
add_executable(test_bm25          tests/test_bm25.cpp)
add_executable(test_tokenizer     tests/test_tokenizer.cpp)
add_executable(test_allocator     tests/test_allocator.cpp)
add_executable(test_thread_pool   tests/test_thread_pool.cpp)
add_executable(test_inverted_list tests/test_inverted_list.cpp src/inverted_list.cpp)
add_executable(test_search_engine tests/test_search_engine.cpp)
add_executable(test_builder       tests/test_builder.cpp)

target_compile_definitions(test_codec         PRIVATE ${IDX_CODEC_DEFINE})
target_compile_definitions(test_inverted_list PRIVATE ${IDX_CODEC_DEFINE})
target_compile_definitions(test_search_engine PRIVATE ${IDX_CODEC_DEFINE})
target_compile_definitions(test_builder       PRIVATE ${IDX_CODEC_DEFINE})

target_link_libraries(test_thread_pool   PRIVATE Threads::Threads)
target_link_libraries(test_search_engine PRIVATE search_engine)
target_link_libraries(test_builder       PRIVATE builder search_engine)

add_test(NAME varbyte       COMMAND test_varbyte)
add_test(NAME raw32         COMMAND test_raw32)
add_test(NAME codec         COMMAND test_codec)
add_test(NAME bm25          COMMAND test_bm25)
add_test(NAME tokenizer     COMMAND test_tokenizer)
add_test(NAME allocator     COMMAND test_allocator)
add_test(NAME thread_pool   COMMAND test_thread_pool)
add_test(NAME inverted_list COMMAND test_inverted_list)
add_test(NAME search_engine COMMAND test_search_engine)
add_test(NAME builder       COMMAND test_builder)

# Python end-to-end test exercising the RAG IPC layer (build_index +
# search_cli --server + Retriever + build_prompt).  Skips gracefully when
# python3 is not on PATH.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME rag_pipeline
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests -p test_rag_pipeline.py -v
    )
    set_tests_properties(rag_pipeline PROPERTIES
        ENVIRONMENT "IDX_BUILD_DIR=${CMAKE_BINARY_DIR}"
        DEPENDS "build_index;search_cli")

    add_test(
        NAME scripts
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests -p test_scripts.py -v
    )

    add_test(
        NAME eval_helpers
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tests -p test_eval.py -v
    )
endif()
