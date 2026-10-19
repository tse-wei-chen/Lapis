cmake_minimum_required(VERSION 3.25)
project(LapisMediaRuntime VERSION 0.15.0 LANGUAGES C CXX)

find_package(FFMPEG REQUIRED)
find_package(Threads REQUIRED)

# Optional adapter. Use vcpkg manifest feature "bluray" when enabling:
# -DLAPIS_ENABLE_LIBBLURAY=ON -DVCPKG_MANIFEST_FEATURES=bluray
option(LAPIS_ENABLE_LIBBLURAY "Build the unprotected BDMV libbluray source" OFF)
if(LAPIS_ENABLE_LIBBLURAY)
    find_path(LIBBLURAY_INCLUDE_DIR NAMES libbluray/bluray.h REQUIRED)
    find_library(LIBBLURAY_LIBRARY NAMES bluray libbluray REQUIRED)
endif()

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
    src/source/bluray/bluray_source.cpp
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

if(LAPIS_ENABLE_LIBBLURAY)
    target_sources(Lapis.MediaRuntime.Core PRIVATE
        src/source/bluray/libbluray_disc.cpp)
    target_include_directories(Lapis.MediaRuntime.Core
        SYSTEM PRIVATE ${LIBBLURAY_INCLUDE_DIR})
    target_link_libraries(Lapis.MediaRuntime.Core
        PRIVATE ${LIBBLURAY_LIBRARY})
    target_compile_definitions(Lapis.MediaRuntime.Core
        PRIVATE LAPIS_ENABLE_LIBBLURAY=1)
endif()

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
# Share the CMake provider choice with the C ABI export implementation.
if(LAPIS_ENABLE_LIBBLURAY)
    target_compile_definitions(Lapis.MediaRuntime PRIVATE LAPIS_ENABLE_LIBBLURAY=1)
endif()
target_include_directories(Lapis.MediaRuntime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(Lapis.MediaRuntime PRIVATE Lapis.MediaRuntime.Core)
lapis_enable_strict_warnings(Lapis.MediaRuntime)

# Windows-only reproducible playback acceptance runner. The runner links through
# the public C ABI (not implementation internals) and is also a manual tool.
if(WIN32)
    add_executable(Lapis.PlaybackSoak tools/windows_playback_soak.cpp)
    target_compile_features(Lapis.PlaybackSoak PRIVATE cxx_std_20)
    target_link_libraries(Lapis.PlaybackSoak PRIVATE
        Lapis.MediaRuntime user32 psapi)
    lapis_enable_strict_warnings(Lapis.PlaybackSoak)
endif()

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
    set(LAPIS_TEST_CHURN_MEDIA_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_soak_churn.mp4")
    set(LAPIS_TEST_Y4M_SEEK_MEDIA_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_seek_y4m.y4m")
    set(LAPIS_TEST_CHURN_ALT_MEDIA_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/test_soak_churn_alt.mp4")

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

    add_executable(Lapis.MediaRuntime.SourceFeatureTests tests/source_feature_flags.c)
    target_compile_features(Lapis.MediaRuntime.SourceFeatureTests PRIVATE c_std_17)
    target_include_directories(Lapis.MediaRuntime.SourceFeatureTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(Lapis.MediaRuntime.SourceFeatureTests PRIVATE Lapis.MediaRuntime)
    if(LAPIS_ENABLE_LIBBLURAY)
        target_compile_definitions(Lapis.MediaRuntime.SourceFeatureTests PRIVATE LAPIS_ENABLE_LIBBLURAY=1)
    endif()
    if(MSVC)
        target_compile_options(Lapis.MediaRuntime.SourceFeatureTests PRIVATE /W4 /WX)
    else()
        target_compile_options(Lapis.MediaRuntime.SourceFeatureTests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME source_features_contract COMMAND Lapis.MediaRuntime.SourceFeatureTests)

    # Pure deterministic contract for the soak runner's growth evaluator.
    # Does not require FFmpeg, D3D11, Windows or a real playback device.
    add_executable(Lapis.MediaRuntime.ResourceBudgetTests
        tests/soak_resource_budget.cpp)
    target_compile_features(Lapis.MediaRuntime.ResourceBudgetTests
        PRIVATE cxx_std_20)
    target_include_directories(Lapis.MediaRuntime.ResourceBudgetTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.ResourceBudgetTests)
    add_test(NAME soak_resource_budget_contract
        COMMAND Lapis.MediaRuntime.ResourceBudgetTests)

    add_executable(Lapis.MediaRuntime.DiagnosticJournalTests
        tests/diagnostic_event_journal.cpp)
    target_compile_features(Lapis.MediaRuntime.DiagnosticJournalTests PRIVATE cxx_std_20)
    target_link_libraries(Lapis.MediaRuntime.DiagnosticJournalTests PRIVATE Threads::Threads)
    target_include_directories(Lapis.MediaRuntime.DiagnosticJournalTests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.DiagnosticJournalTests)
    add_test(NAME diagnostic_journal_concurrency
        COMMAND Lapis.MediaRuntime.DiagnosticJournalTests)

    # This fixture tests the future libbluray boundary without linking libbluray.
    add_executable(Lapis.MediaRuntime.BluraySourceTests
        tests/bluray_source.cpp)
    target_compile_features(Lapis.MediaRuntime.BluraySourceTests
        PRIVATE cxx_std_20)
    target_compile_definitions(Lapis.MediaRuntime.BluraySourceTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}")
    target_link_libraries(Lapis.MediaRuntime.BluraySourceTests
        PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.BluraySourceTests)
    add_test(NAME bluray_source_contract
        COMMAND Lapis.MediaRuntime.BluraySourceTests)

    add_executable(Lapis.MediaRuntime.NavigationContractTests
        tests/navigation_contract.cpp)
    target_compile_features(Lapis.MediaRuntime.NavigationContractTests
        PRIVATE cxx_std_20)
    target_compile_definitions(Lapis.MediaRuntime.NavigationContractTests
        PRIVATE LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}")
    target_link_libraries(Lapis.MediaRuntime.NavigationContractTests
        PRIVATE Lapis.MediaRuntime.Core)
    lapis_enable_strict_warnings(Lapis.MediaRuntime.NavigationContractTests)
    add_test(NAME navigation_contract
        COMMAND Lapis.MediaRuntime.NavigationContractTests)

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
        PRIVATE
            LAPIS_TEST_MEDIA_PATH="${LAPIS_TEST_MEDIA_PATH}"
            LAPIS_TEST_NONZERO_SEEK_MEDIA_PATH="${LAPIS_TEST_CHURN_MEDIA_PATH}"
            LAPIS_TEST_Y4M_SEEK_MEDIA_PATH="${LAPIS_TEST_Y4M_SEEK_MEDIA_PATH}"
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
        # Small continuous integration sample: two EOF passes and two early
        # closures exercise the actual D3D11/FFmpeg/native C ABI lifecycle.
        # This is NOT the multi-hour physical Windows acceptance suite.
        add_test(
            NAME windows_native_playback_soak_quick
            COMMAND Lapis.PlaybackSoak
                --media "${LAPIS_TEST_MULTI_FRAME_MEDIA_PATH}"
                --cycles 4
                --mode mixed
                --timeout-seconds 10
        )
        set_tests_properties(windows_native_playback_soak_quick
            PROPERTIES TIMEOUT 60 LABELS "windows;soak;quick")

        # Post-warmup peak growth (every post-Close sample), including EOF
        # and early-Close GPU paths. These budgets are deliberately loose CI
        # smoke limits, not proof of multi-hour leak freedom.
        add_test(
            NAME windows_native_resource_growth_quick
            COMMAND Lapis.PlaybackSoak
                --media "${LAPIS_TEST_MULTI_FRAME_MEDIA_PATH}"
                --cycles 20
                --warmup-cycles 4
                --mode mixed
                --max-handle-growth 64
                --max-gdi-growth 32
                --max-user-growth 32
                --max-private-growth-mib 128
                --timeout-seconds 10
        )
        set_tests_properties(windows_native_resource_growth_quick
            PROPERTIES TIMEOUT 90 LABELS "windows;soak;resources")

        # Six-second indexed MPEG-4 clips keep the real renderer Running long
        # enough to verify active HWND rebinding and two epoch-advancing seeks.
        # Alternate distinct source files to exercise content switching.
        add_test(
            NAME windows_native_playback_churn_quick
            COMMAND Lapis.PlaybackSoak
                --media "${LAPIS_TEST_CHURN_MEDIA_PATH}"
                --media-alternate "${LAPIS_TEST_CHURN_ALT_MEDIA_PATH}"
                --cycles 2
                --mode churn
                --seek-ms 250
                --timeout-seconds 10
        )
        set_tests_properties(windows_native_playback_churn_quick
            PROPERTIES TIMEOUT 75 LABELS "windows;soak;quick;churn")

        # Verify the FFmpeg custom-AVIO Y4M fallback at the full C ABI and
        # live D3D HWND layer, not only at the demuxer unit boundary.
        add_test(
            NAME windows_native_playback_y4m_seek_quick
            COMMAND Lapis.PlaybackSoak
                --media "${LAPIS_TEST_Y4M_SEEK_MEDIA_PATH}"
                --cycles 1
                --mode churn
                --seek-ms 250
                --timeout-seconds 10
        )
        set_tests_properties(windows_native_playback_y4m_seek_quick
            PROPERTIES TIMEOUT 45 LABELS "windows;soak;quick;seek")

        # Exercise WinForms-like persistent output HWND parenting while
        # a REAL D3D swap-chain remains attached to that unchanged child HWND.
        # Unlike --mode churn, this must never rebind the native renderer.
        add_test(
            NAME windows_native_playback_parent_churn_quick
            COMMAND Lapis.PlaybackSoak
                --media "${LAPIS_TEST_CHURN_MEDIA_PATH}"
                --cycles 1
                --mode parent-churn
                --parent-recreates 16
                --timeout-seconds 15
        )
        set_tests_properties(windows_native_playback_parent_churn_quick
            PROPERTIES TIMEOUT 40 LABELS "windows;soak;quick;hwnd")

        # Full FFmpeg -> PlaybackExecution -> real D3D11 presenter fault
        # injection. The test-only decorator synthesizes DeviceLost while
        # real D3D resources are released/rebuilt by the production worker.
        # No synthetic device-loss switch is exposed in the player DLL.
        add_executable(
            Lapis.MediaRuntime.D3D11RecoveryInjectionTests
            tests/d3d11_device_loss_injection.cpp
        )
        target_compile_features(
            Lapis.MediaRuntime.D3D11RecoveryInjectionTests PRIVATE cxx_std_20
        )
        target_compile_definitions(
            Lapis.MediaRuntime.D3D11RecoveryInjectionTests PRIVATE
                LAPIS_TEST_CHURN_MEDIA_PATH="${LAPIS_TEST_CHURN_MEDIA_PATH}"
                LAPIS_TEST_TWO_FRAME_MEDIA_PATH="${LAPIS_TEST_MULTI_FRAME_MEDIA_PATH}"
        )
        target_link_libraries(
            Lapis.MediaRuntime.D3D11RecoveryInjectionTests PRIVATE
                Lapis.MediaRuntime.Core user32
        )
        lapis_enable_strict_warnings(
            Lapis.MediaRuntime.D3D11RecoveryInjectionTests
        )
        add_test(
            NAME d3d11_live_device_loss_recovery_injection
            COMMAND Lapis.MediaRuntime.D3D11RecoveryInjectionTests
        )
        set_tests_properties(d3d11_live_device_loss_recovery_injection
            PROPERTIES TIMEOUT 45 LABELS "windows;gpu;injection")

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

        # Kernel-event wait test: an invalidated WASAPI endpoint may stop
        # signalling AUDIO_EVENT entirely. Assert a bounded polling probe
        # detects loss and Stop/Seek still interrupts immediately via event.
        add_executable(
            Lapis.MediaRuntime.WasapiEventWaitTests
            tests/wasapi_event_wait.cpp
        )
        target_compile_features(
            Lapis.MediaRuntime.WasapiEventWaitTests PRIVATE cxx_std_20
        )
        target_include_directories(
            Lapis.MediaRuntime.WasapiEventWaitTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(
            Lapis.MediaRuntime.WasapiEventWaitTests PRIVATE user32
        )
        lapis_enable_strict_warnings(
            Lapis.MediaRuntime.WasapiEventWaitTests
        )
        add_test(
            NAME wasapi_endpoint_loss_event_wait
            COMMAND Lapis.MediaRuntime.WasapiEventWaitTests
        )
        set_tests_properties(wasapi_endpoint_loss_event_wait
            PROPERTIES TIMEOUT 15 LABELS "windows;audio;injection")

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
