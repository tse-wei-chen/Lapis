cmake_minimum_required(VERSION 3.25)
project(LapisMediaRuntime VERSION 0.10.0 LANGUAGES C CXX)

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
    src/core/playback_clock.cpp
    src/source/media_source_registry.cpp
    src/source/file/file_source.cpp
    src/demux/ffmpeg/ffmpeg_io.cpp
    src/demux/ffmpeg/ffmpeg_demuxer.cpp
    src/demux/ffmpeg/ffmpeg_packet_router.cpp
    src/decode/ffmpeg/ffmpeg_video_decoder.cpp
    src/decode/ffmpeg/ffmpeg_audio_decoder.cpp
    src/process/ffmpeg/ffmpeg_video_frame_converter.cpp
    src/execution/video_decode_worker.cpp
    src/execution/video_process_worker.cpp
    src/execution/video_scheduler_worker.cpp
    src/execution/video_render_worker.cpp
    src/execution/playback_execution.cpp
    src/session/playback_media_session.cpp
    src/video/video_scheduler.cpp
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
    set(
        LAPIS_TEST_MULTI_FRAME_MEDIA_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_two_frames.y4m"
    )
    set(
        LAPIS_TEST_PIPELINE_MEDIA_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_pipeline.y4m"
    )
    set(LAPIS_TEST_AUDIO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_audio.wav")

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

    add_executable(Lapis.MediaRuntime.PacketRouterTests tests/ffmpeg_packet_router.cpp)
    target_compile_features(Lapis.MediaRuntime.PacketRouterTests PRIVATE cxx_std_20)
    target_include_directories(
        Lapis.MediaRuntime.PacketRouterTests
        SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_libraries(Lapis.MediaRuntime.PacketRouterTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.PacketRouterTests)
    add_test(NAME ffmpeg_packet_router_smoke COMMAND Lapis.MediaRuntime.PacketRouterTests)

    add_executable(Lapis.MediaRuntime.AudioDecodeTests tests/ffmpeg_audio_decode.cpp)
    target_compile_features(Lapis.MediaRuntime.AudioDecodeTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.AudioDecodeTests
        PRIVATE LAPIS_TEST_AUDIO_PATH="${LAPIS_TEST_AUDIO_PATH}"
    )
    target_include_directories(
        Lapis.MediaRuntime.AudioDecodeTests
        SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_libraries(Lapis.MediaRuntime.AudioDecodeTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioDecodeTests)
    add_test(NAME ffmpeg_audio_decode_smoke COMMAND Lapis.MediaRuntime.AudioDecodeTests)

    add_executable(Lapis.MediaRuntime.VideoProcessingTests tests/video_processing.cpp)
    target_compile_features(Lapis.MediaRuntime.VideoProcessingTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.VideoProcessingTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.VideoProcessingTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.VideoProcessingTests)
    add_test(NAME video_processing_smoke COMMAND Lapis.MediaRuntime.VideoProcessingTests)

    add_executable(Lapis.MediaRuntime.ExecutionTests tests/execution_pipeline.cpp)
    target_compile_features(Lapis.MediaRuntime.ExecutionTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.ExecutionTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.ExecutionTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.ExecutionTests)
    add_test(NAME execution_pipeline_smoke COMMAND Lapis.MediaRuntime.ExecutionTests)

    add_executable(
        Lapis.MediaRuntime.ClockSchedulerTests
        tests/playback_clock_scheduler.cpp
    )
    target_compile_features(Lapis.MediaRuntime.ClockSchedulerTests PRIVATE cxx_std_20)
    target_link_libraries(
        Lapis.MediaRuntime.ClockSchedulerTests
        PRIVATE Lapis.MediaRuntime.Core
    )
    lapis_enable_strict_warnings(Lapis.MediaRuntime.ClockSchedulerTests)
    add_test(
        NAME playback_clock_scheduler_smoke
        COMMAND Lapis.MediaRuntime.ClockSchedulerTests
    )

    add_executable(
        Lapis.MediaRuntime.VideoSchedulerExecutionTests
        tests/video_scheduler_execution.cpp
    )
    target_compile_features(
        Lapis.MediaRuntime.VideoSchedulerExecutionTests
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        Lapis.MediaRuntime.VideoSchedulerExecutionTests
        PRIVATE LAPIS_TEST_MULTI_FRAME_MEDIA_PATH="${LAPIS_TEST_MULTI_FRAME_MEDIA_PATH}"
    )
    target_link_libraries(
        Lapis.MediaRuntime.VideoSchedulerExecutionTests
        PRIVATE Lapis.MediaRuntime.Core
    )
    lapis_enable_strict_warnings(Lapis.MediaRuntime.VideoSchedulerExecutionTests)
    add_test(
        NAME video_scheduler_execution_smoke
        COMMAND Lapis.MediaRuntime.VideoSchedulerExecutionTests
    )

    add_executable(
        Lapis.MediaRuntime.VideoRenderExecutionTests
        tests/video_render_execution.cpp
    )
    target_compile_features(
        Lapis.MediaRuntime.VideoRenderExecutionTests
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        Lapis.MediaRuntime.VideoRenderExecutionTests
        PRIVATE LAPIS_TEST_MULTI_FRAME_MEDIA_PATH="${LAPIS_TEST_MULTI_FRAME_MEDIA_PATH}"
    )
    target_link_libraries(
        Lapis.MediaRuntime.VideoRenderExecutionTests
        PRIVATE Lapis.MediaRuntime.Core
    )
    lapis_enable_strict_warnings(Lapis.MediaRuntime.VideoRenderExecutionTests)
    add_test(
        NAME video_render_execution_smoke
        COMMAND Lapis.MediaRuntime.VideoRenderExecutionTests
    )

    add_executable(
        Lapis.MediaRuntime.PlaybackExecutionTests
        tests/playback_execution.cpp
    )
    target_compile_features(
        Lapis.MediaRuntime.PlaybackExecutionTests
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        Lapis.MediaRuntime.PlaybackExecutionTests
        PRIVATE LAPIS_TEST_PIPELINE_MEDIA_PATH="${LAPIS_TEST_PIPELINE_MEDIA_PATH}"
    )
    target_link_libraries(
        Lapis.MediaRuntime.PlaybackExecutionTests
        PRIVATE Lapis.MediaRuntime.Core
    )
    lapis_enable_strict_warnings(Lapis.MediaRuntime.PlaybackExecutionTests)
    add_test(
        NAME playback_execution_smoke
        COMMAND Lapis.MediaRuntime.PlaybackExecutionTests
    )

    add_executable(Lapis.MediaRuntime.BoundedQueueTests tests/bounded_queue.cpp)
    target_compile_features(Lapis.MediaRuntime.BoundedQueueTests PRIVATE cxx_std_20)
    target_link_libraries(Lapis.MediaRuntime.BoundedQueueTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.BoundedQueueTests)
    add_test(NAME bounded_queue_concurrency COMMAND Lapis.MediaRuntime.BoundedQueueTests)
endif()
