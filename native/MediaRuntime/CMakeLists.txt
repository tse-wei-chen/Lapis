cmake_minimum_required(VERSION 3.25)
project(LapisMediaRuntime VERSION 0.15.0 LANGUAGES C CXX)

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
    src/process/ffmpeg/ffmpeg_audio_frame_converter.cpp
    src/process/ffmpeg/ffmpeg_video_frame_converter.cpp
    src/execution/video_decode_worker.cpp
    src/execution/audio_clock_discipline.cpp
    src/execution/audio_clock_sync_worker.cpp
    src/execution/audio_decode_worker.cpp
    src/execution/audio_process_worker.cpp
    src/execution/audio_render_worker.cpp
    src/execution/video_process_worker.cpp
    src/execution/video_scheduler_worker.cpp
    src/execution/video_render_worker.cpp
    src/execution/playback_execution.cpp
    src/session/playback_media_session.cpp
    src/video/video_scheduler.cpp
)

if(WIN32)
    target_sources(Lapis.MediaRuntime.Core PRIVATE
        src/audio/wasapi/wasapi_format.cpp
        src/audio/wasapi/wasapi_shared_audio_output.cpp
        src/audio/wasapi/wasapi_shared_audio_output_provider.cpp
        src/video/d3d11/d3d11_video_presenter.cpp
    )
endif()

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
if(WIN32)
    target_link_libraries(Lapis.MediaRuntime.Core PRIVATE ole32 d3d11 dxgi d3dcompiler user32)
endif()
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
    if(WIN32)
        target_link_libraries(Lapis.MediaRuntime.SmokeTests PRIVATE user32)
    endif()
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

    add_executable(Lapis.MediaRuntime.AudioExecutionTests tests/audio_decode_execution.cpp)
    target_compile_features(Lapis.MediaRuntime.AudioExecutionTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.AudioExecutionTests
        PRIVATE LAPIS_TEST_AUDIO_PATH="${LAPIS_TEST_AUDIO_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.AudioExecutionTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioExecutionTests)
    add_test(NAME audio_decode_execution_smoke COMMAND Lapis.MediaRuntime.AudioExecutionTests)

    add_executable(Lapis.MediaRuntime.AudioProcessingTests tests/audio_processing.cpp)
    target_compile_features(Lapis.MediaRuntime.AudioProcessingTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.AudioProcessingTests
        PRIVATE LAPIS_TEST_AUDIO_PATH="${LAPIS_TEST_AUDIO_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.AudioProcessingTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioProcessingTests)
    add_test(NAME audio_processing_smoke COMMAND Lapis.MediaRuntime.AudioProcessingTests)

    add_executable(Lapis.MediaRuntime.AudioRenderTests tests/audio_render_execution.cpp)
    target_compile_features(Lapis.MediaRuntime.AudioRenderTests PRIVATE cxx_std_20)
    target_compile_definitions(
        Lapis.MediaRuntime.AudioRenderTests
        PRIVATE LAPIS_TEST_AUDIO_PATH="${LAPIS_TEST_AUDIO_PATH}"
    )
    target_link_libraries(Lapis.MediaRuntime.AudioRenderTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioRenderTests)
    add_test(NAME audio_render_execution_smoke COMMAND Lapis.MediaRuntime.AudioRenderTests)

    add_executable(
        Lapis.MediaRuntime.AudioClockDisciplineTests
        tests/audio_clock_discipline.cpp
    )
    target_compile_features(
        Lapis.MediaRuntime.AudioClockDisciplineTests PRIVATE cxx_std_20
    )
    target_link_libraries(
        Lapis.MediaRuntime.AudioClockDisciplineTests
        PRIVATE Lapis.MediaRuntime.Core
    )
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioClockDisciplineTests)
    add_test(
        NAME audio_clock_discipline_contract
        COMMAND Lapis.MediaRuntime.AudioClockDisciplineTests
    )

    add_executable(
        Lapis.MediaRuntime.AudioClockSyncTests
        tests/audio_clock_sync_execution.cpp
    )
    target_compile_features(
        Lapis.MediaRuntime.AudioClockSyncTests PRIVATE cxx_std_20
    )
    target_link_libraries(
        Lapis.MediaRuntime.AudioClockSyncTests
        PRIVATE Lapis.MediaRuntime.Core
    )
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioClockSyncTests)
    add_test(
        NAME audio_clock_sync_execution
        COMMAND Lapis.MediaRuntime.AudioClockSyncTests
    )

    if(WIN32)
        add_executable(
            Lapis.MediaRuntime.D3D11PresenterTests
            tests/d3d11_video_presenter.cpp
        )
        target_compile_features(
            Lapis.MediaRuntime.D3D11PresenterTests PRIVATE cxx_std_20
        )
        target_link_libraries(
            Lapis.MediaRuntime.D3D11PresenterTests PRIVATE Lapis.MediaRuntime.Core user32
        )
        lapis_enable_strict_warnings(Lapis.MediaRuntime.D3D11PresenterTests)
        add_test(
            NAME d3d11_video_presenter_smoke
            COMMAND Lapis.MediaRuntime.D3D11PresenterTests
        )

        add_executable(
            Lapis.MediaRuntime.WasapiFormatTests
            tests/wasapi_audio_output.cpp
        )
        target_compile_features(
            Lapis.MediaRuntime.WasapiFormatTests PRIVATE cxx_std_20
        )
        target_link_libraries(
            Lapis.MediaRuntime.WasapiFormatTests
            PRIVATE Lapis.MediaRuntime.Core
        )
        lapis_enable_strict_warnings(Lapis.MediaRuntime.WasapiFormatTests)
        add_test(
            NAME wasapi_audio_format_contract
            COMMAND Lapis.MediaRuntime.WasapiFormatTests
        )
    endif()

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
        Lapis.MediaRuntime.VideoSchedulerExecutionTests PRIVATE cxx_std_20
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
        Lapis.MediaRuntime.VideoRenderExecutionTests PRIVATE cxx_std_20
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
        Lapis.MediaRuntime.PlaybackExecutionTests PRIVATE cxx_std_20
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

    add_executable(Lapis.MediaRuntime.AudioOutputContractTests tests/audio_output_contract.cpp)
    target_compile_features(Lapis.MediaRuntime.AudioOutputContractTests PRIVATE cxx_std_20)
    target_link_libraries(Lapis.MediaRuntime.AudioOutputContractTests PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.AudioOutputContractTests)
    add_test(NAME audio_output_contract COMMAND Lapis.MediaRuntime.AudioOutputContractTests)
endif()
