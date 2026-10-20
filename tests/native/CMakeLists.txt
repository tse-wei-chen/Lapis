add_executable(bdmv_native_smoke smoke.cpp)
target_link_libraries(bdmv_native_smoke PRIVATE MediaRuntime)
target_compile_features(bdmv_native_smoke PRIVATE cxx_std_20)
add_test(NAME native_smoke COMMAND bdmv_native_smoke)

add_executable(bdmv_c_consumer c_consumer.c)
target_link_libraries(bdmv_c_consumer PRIVATE MediaRuntime)
# The consumer itself is strict C; the shared library is C++. Use the C++
# linker driver so Clang UBSan's C++ runtime can be resolved in sanitizer builds.
set_target_properties(bdmv_c_consumer PROPERTIES LINKER_LANGUAGE CXX)
add_test(NAME c_abi_layout COMMAND bdmv_c_consumer)

add_executable(bdmv_core_contract_compile core_contract_compile.cpp)
target_include_directories(bdmv_core_contract_compile PRIVATE
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)
add_test(NAME cpp_contract_compile COMMAND bdmv_core_contract_compile)
target_link_libraries(bdmv_core_contract_compile PRIVATE MediaRuntime)

add_executable(bdmv_media_foundations media_foundations.cpp
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src/adapters/file/file_source.cpp)
target_include_directories(bdmv_media_foundations PRIVATE
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)
target_link_libraries(bdmv_media_foundations PRIVATE MediaRuntime)
add_test(NAME media_foundations COMMAND bdmv_media_foundations)

add_executable(bdmv_file_probe file_probe.cpp
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src/adapters/file/file_source.cpp)
target_include_directories(bdmv_file_probe PRIVATE
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)

add_executable(bdmv_decoded_contracts decoded_contracts.cpp)
target_include_directories(bdmv_decoded_contracts PRIVATE
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)
add_test(NAME decoded_contracts COMMAND bdmv_decoded_contracts)

if(BDMV_WITH_FFMPEG)
    add_executable(bdmv_ffmpeg_decode ffmpeg_decode.cpp)
    target_link_libraries(bdmv_ffmpeg_decode PRIVATE BdmvFfmpegAdapter)
    add_test(NAME ffmpeg_adapter_fixture
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/ffmpeg_adapter_fixture.py"
                "$<TARGET_FILE:bdmv_ffmpeg_decode>" "$<TARGET_FILE:bdmv_frame_dump>")
    # If FFmpeg CLI/Python are unavailable, adapter can still be built and used.
    if(NOT Python3_Interpreter_FOUND OR NOT BDMV_FFMPEG_TOOL)
        set_tests_properties(ffmpeg_adapter_fixture PROPERTIES DISABLED TRUE)
    endif()
endif()

if(BDMV_ENABLE_TEST_SOURCE)
    add_executable(bdmv_decoder_pipeline decoder_pipeline.cpp)
    target_include_directories(bdmv_decoder_pipeline PRIVATE
        ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)
    target_link_libraries(bdmv_decoder_pipeline PRIVATE MediaRuntime)
    # DecodePump is compiled into MediaRuntime with hidden visibility; link this
    # standalone test against the same portable source, not private ABI symbols.
    target_sources(bdmv_decoder_pipeline PRIVATE
        ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src/core/decode_pump.cpp)
    add_test(NAME decoder_pipeline COMMAND bdmv_decoder_pipeline)
endif()

add_executable(bdmv_frame_dump ${PROJECT_SOURCE_DIR}/tools/bdmv_frame_dump.cpp)
target_link_libraries(bdmv_frame_dump PRIVATE MediaRuntime)
if(BDMV_ENABLE_TEST_SOURCE)
    add_test(NAME synthetic_frame_dump
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_frame_dump.py"
                "$<TARGET_FILE:bdmv_frame_dump>" "${CMAKE_CURRENT_BINARY_DIR}")
    if(NOT Python3_Interpreter_FOUND)
        set_tests_properties(synthetic_frame_dump PROPERTIES DISABLED TRUE)
    endif()
endif()

add_executable(bdmv_presentation_timing presentation_timing.cpp)
target_include_directories(bdmv_presentation_timing PRIVATE
    ${PROJECT_SOURCE_DIR}/native/MediaRuntime/src)
add_test(NAME presentation_timing COMMAND bdmv_presentation_timing)

# Headless preview test is opt-in because SDL2 runtime is not part of the SDK.
if(BDMV_BUILD_PREVIEW AND BDMV_TEST_SDL_DUMMY AND BDMV_ENABLE_TEST_SOURCE
   AND UNIX AND NOT APPLE AND Python3_Interpreter_FOUND)
    add_test(NAME sdl_dummy_presentation
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_sdl_preview.py"
            "$<TARGET_FILE:bdmv_sdl_preview>")
endif()
