cmake_minimum_required(VERSION 3.24)
project(BdmvPlayer VERSION 0.2.0 LANGUAGES C CXX)

option(BDMV_BUILD_TESTS "Build native and architecture tests" ON)
option(BDMV_WITH_FFMPEG "Build optional in-process FFmpeg software decoder (requires dev libraries)" OFF)
option(BDMV_ENABLE_TEST_SOURCE "Enable non-media synthetic diagnostic source" ON)
option(BDMV_BUILD_PREVIEW "Build SDL2 developer preview (runtime-loaded SDL2, not production UI)" OFF)
option(BDMV_TEST_SDL_DUMMY "Run SDL2 dummy video/audio integration test (requires SDL2 runtime)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(native/MediaRuntime)
if(BDMV_BUILD_PREVIEW)
    add_executable(bdmv_sdl_preview tools/bdmv_sdl_preview.cpp)
    target_include_directories(bdmv_sdl_preview PRIVATE
        ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)
    target_link_libraries(bdmv_sdl_preview PRIVATE MediaRuntime ${CMAKE_DL_LIBS})
endif()
if(BDMV_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_subdirectory(tests/native)
    if(Python3_Interpreter_FOUND)
        add_test(NAME architecture_boundaries
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture/test_boundaries.py")
        # Tool-level fixture validation only. Does not claim runtime media playback.
        find_program(BDMV_FFMPEG_TOOL ffmpeg)
        find_program(BDMV_FFPROBE_TOOL ffprobe)
        if(BDMV_FFMPEG_TOOL AND BDMV_FFPROBE_TOOL)
            add_test(NAME ffmpeg_cli_diagnostic
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg_media_diagnostic.py"
                        --self-test --native-probe "$<TARGET_FILE:bdmv_file_probe>")
        endif()
    endif()
endif()
