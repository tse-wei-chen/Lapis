cmake_minimum_required(VERSION 3.25)
project(LapisMediaRuntime VERSION 0.3.0 LANGUAGES C CXX)

find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)

function(lapis_enable_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

add_library(Lapis.MediaRuntime.Core STATIC
    src/core/graph_builder.cpp
    src/source/media_source_registry.cpp
    src/source/file/file_source.cpp
    src/demux/ffmpeg/ffmpeg_io.cpp
    src/demux/ffmpeg/ffmpeg_demuxer.cpp
    src/decode/ffmpeg/ffmpeg_video_decoder.cpp
    src/execution/video_decode_worker.cpp
    src/session/playback_media_session.cpp
)

target_compile_features(Lapis.MediaRuntime.Core PUBLIC cxx_std_20)
set_target_properties(Lapis.MediaRuntime.Core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(Lapis.MediaRuntime.Core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)
target_include_directories(Lapis.MediaRuntime.Core SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(Lapis.MediaRuntime.Core PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(Lapis.MediaRuntime.Core
    PUBLIC Threads::Threads
    PRIVATE ${FFMPEG_LIBRARIES}
)
lapis_enable_strict_warnings(Lapis.MediaRuntime.Core)

add_library(Lapis.MediaRuntime SHARED
    src/media_runtime.cpp
)

target_compile_features(Lapis.MediaRuntime PRIVATE cxx_std_20)
target_compile_definitions(Lapis.MediaRuntime PRIVATE LAPIS_MEDIA_RUNTIME_EXPORTS)
target_include_directories(Lapis.MediaRuntime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(Lapis.MediaRuntime PRIVATE Lapis.MediaRuntime.Core)
lapis_enable_strict_warnings(Lapis.MediaRuntime)

include(CTest)
if(BUILD_TESTING)
    set(LAPIS_TEST_MEDIA_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test.y4m")

    add_executable(Lapis.MediaRuntime.CAbiHeaderTests tests/c_abi_header.c)
    target_compile_features(Lapis.MediaRuntime.CAbiHeaderTests PRIVATE c_std_17)
    target_include_directories(
        Lapis.MediaRuntime.CAbiHeaderTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(MSVC)
        target_compile_options(Lapis.MediaRuntime.CAbiHeaderTests PRIVATE /W4 /WX)
    else()
        target_compile_options(
            Lapis.MediaRuntime.CAbiHeaderTests
            PRIVATE -Wall -Wextra -Wpedantic -Werror
        )
    endif()
    add_test(NAME c_abi_header_smoke COMMAND Lapis.MediaRuntime.CAbiHeaderTests)

    add_executable(Lapis.MediaRuntime.GraphPlanTests tests/graph_plan.cpp)
    target_compile_features(Lapis.MediaRuntime.GraphPlanTests PRIVATE cxx_std_20)
    target_link_libraries(Lapis.MediaRuntime.GraphPlanTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.GraphPlanTests)
    add_test(NAME graph_plan_smoke COMMAND Lapis.MediaRuntime.GraphPlanTests)

    add_executable(Lapis.MediaRuntime.SmokeTests tests/smoke.cpp)
    target_compile_features(Lapis.MediaRuntime.SmokeTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.SmokeTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.SmokeTests PRIVATE Lapis.MediaRuntime)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.SmokeTests)
    add_test(NAME media_runtime_smoke COMMAND Lapis.MediaRuntime.SmokeTests)

    add_executable(Lapis.MediaRuntime.FfmpegTests tests/ffmpeg_decode.cpp)
    target_compile_features(Lapis.MediaRuntime.FfmpegTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.FfmpegTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.FfmpegTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.FfmpegTests)
    add_test(NAME ffmpeg_decode_smoke COMMAND Lapis.MediaRuntime.FfmpegTests)

    add_executable(Lapis.MediaRuntime.ExecutionTests tests/execution_pipeline.cpp)
    target_compile_features(Lapis.MediaRuntime.ExecutionTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.ExecutionTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.ExecutionTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.ExecutionTests)
    add_test(NAME execution_pipeline_smoke COMMAND Lapis.MediaRuntime.ExecutionTests)
endif()
