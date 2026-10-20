add_library(MediaRuntime SHARED
    src/core/session.cpp
    src/core/decode_pump.cpp
    src/interop/player_c_api.cpp
    src/adapters/file/file_source.cpp
)

target_include_directories(MediaRuntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(MediaRuntime PRIVATE src)
target_compile_definitions(MediaRuntime PRIVATE BDMV_RUNTIME_EXPORTS)
if(BDMV_ENABLE_TEST_SOURCE)
    target_sources(MediaRuntime PRIVATE src/adapters/synthetic_source.cpp)
    target_compile_definitions(MediaRuntime PRIVATE BDMV_ENABLE_TEST_SOURCE=1)
endif()

if(MSVC)
    target_compile_options(MediaRuntime PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(MediaRuntime PRIVATE -Wall -Wextra -Wpedantic -Werror)
    set_target_properties(MediaRuntime PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
endif()

set_target_properties(MediaRuntime PROPERTIES
    OUTPUT_NAME MediaRuntime
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

# Optional in-process decode backend. Never link libav* into portable core headers.
# Dependencies are discovered via pkg-config on Linux and vcpkg/MSYS2 on Windows.
# Without development headers, MediaRuntime stays buildable without pretending to decode.
if(BDMV_WITH_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(BDMV_AV REQUIRED IMPORTED_TARGET
        libavformat>=60 libavcodec>=60 libavutil>=58 libswscale>=7 libswresample>=4)
    add_library(BdmvFfmpegAdapter STATIC
        src/adapters/ffmpeg/ffmpeg_decoder.cpp)
    target_include_directories(BdmvFfmpegAdapter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(BdmvFfmpegAdapter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(BdmvFfmpegAdapter PUBLIC PkgConfig::BDMV_AV)
    target_compile_features(BdmvFfmpegAdapter PUBLIC cxx_std_20)
    set_target_properties(BdmvFfmpegAdapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(MediaRuntime PRIVATE BdmvFfmpegAdapter)
    target_compile_definitions(MediaRuntime PRIVATE BDMV_WITH_FFMPEG=1)
    if(MSVC)
        target_compile_options(BdmvFfmpegAdapter PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(BdmvFfmpegAdapter PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()
