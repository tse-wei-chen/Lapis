cmake_minimum_required(VERSION 3.25)
project(LapisMediaRuntime VERSION 0.1.0 LANGUAGES CXX)

add_library(Lapis.MediaRuntime SHARED
    src/media_runtime.cpp
)

target_compile_features(Lapis.MediaRuntime PRIVATE cxx_std_20)
target_compile_definitions(Lapis.MediaRuntime PRIVATE LAPIS_MEDIA_RUNTIME_EXPORTS)
target_include_directories(Lapis.MediaRuntime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
    target_compile_options(Lapis.MediaRuntime PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(Lapis.MediaRuntime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(Lapis.MediaRuntime.SmokeTests tests/smoke.cpp)
    target_compile_features(Lapis.MediaRuntime.SmokeTests PRIVATE cxx_std_20)
    target_link_libraries(Lapis.MediaRuntime.SmokeTests PRIVATE Lapis.MediaRuntime)
    add_test(NAME media_runtime_smoke COMMAND Lapis.MediaRuntime.SmokeTests)
endif()
