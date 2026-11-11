cmake_minimum_required(VERSION 3.24)
project(AIMouse VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "AIMouse requires 64-bit Windows")
endif()

include(FetchContent)
find_package(nlohmann_json 3.12.0 EXACT QUIET)
if(NOT nlohmann_json_FOUND)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
        URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(AIMouse WIN32 src/main.cpp src/session.cpp src/mcp.cpp src/capture.cpp)
target_compile_features(AIMouse PRIVATE cxx_std_20)
target_compile_definitions(AIMouse PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(AIMouse PRIVATE nlohmann_json::nlohmann_json user32 gdi32 dwmapi
    shell32 d3d11 dxgi windowscodecs runtimeobject ole32 crypt32 advapi32)
if(MSVC)
    target_compile_options(AIMouse PRIVATE /W4 /EHsc /utf-8 /permissive-)
    set_property(TARGET AIMouse PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests/probes)
    install(TARGETS aimouse_input_probe RUNTIME DESTINATION tools)
    find_program(NODE_EXECUTABLE node)
    if(NODE_EXECUTABLE)
        add_executable(aimouse_test_host tests/host_snapshot.cpp)
        target_compile_features(aimouse_test_host PRIVATE cxx_std_20)
        target_link_libraries(aimouse_test_host PRIVATE user32)
        add_test(NAME aimouse_mcp
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp.mjs"
                "$<TARGET_FILE:AIMouse>" "$<TARGET_FILE:aimouse_input_probe>" "$<TARGET_FILE:aimouse_test_host>")
        set_tests_properties(aimouse_mcp PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
    endif()
endif()

install(TARGETS AIMouse RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")
install(DIRECTORY examples/ DESTINATION examples FILES_MATCHING PATTERN "*.json" PATTERN "*.toml")
install(DIRECTORY analysis/evidence/ DESTINATION analysis/evidence FILES_MATCHING PATTERN "*.json")
install(DIRECTORY analysis/callgraphs/ DESTINATION analysis/callgraphs FILES_MATCHING PATTERN "*.md")
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "AIMouse-${PROJECT_VERSION}-windows-x64")
include(CPack)
