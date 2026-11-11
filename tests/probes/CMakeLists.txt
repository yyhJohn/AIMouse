add_executable(aimouse_input_probe win32_probe.cpp)
target_compile_features(aimouse_input_probe PRIVATE cxx_std_20)
target_compile_definitions(aimouse_input_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(aimouse_input_probe PRIVATE user32 gdi32 dinput8 dxguid)
if(MSVC)
    target_compile_options(aimouse_input_probe PRIVATE /W4 /utf-8 /permissive-)
    set_property(TARGET aimouse_input_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(AIMOUSE_BUILD_ENGINE_PROBES "Build SDL 3 and GLFW input receiver probes" OFF)
if(AIMOUSE_BUILD_ENGINE_PROBES)
    find_package(SDL3 3.2 CONFIG REQUIRED)
    find_package(glfw3 3.4 CONFIG QUIET)
    if(NOT TARGET glfw)
        set(AIMOUSE_GLFW_ROOT "" CACHE PATH "Extracted GLFW 3.4 Windows x64 release")
        find_path(AIMOUSE_GLFW_INCLUDE_DIR GLFW/glfw3.h HINTS "${AIMOUSE_GLFW_ROOT}/include" REQUIRED)
        find_library(AIMOUSE_GLFW_LIBRARY glfw3dll HINTS "${AIMOUSE_GLFW_ROOT}/lib-vc2022" REQUIRED)
        find_file(AIMOUSE_GLFW_DLL glfw3.dll HINTS "${AIMOUSE_GLFW_ROOT}/lib-vc2022" REQUIRED)
        add_library(glfw SHARED IMPORTED)
        set_target_properties(glfw PROPERTIES
            IMPORTED_LOCATION "${AIMOUSE_GLFW_DLL}"
            IMPORTED_IMPLIB "${AIMOUSE_GLFW_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${AIMOUSE_GLFW_INCLUDE_DIR}"
            INTERFACE_COMPILE_DEFINITIONS GLFW_DLL)
    endif()

    add_executable(aimouse_sdl_probe sdl_probe.cpp)
    target_link_libraries(aimouse_sdl_probe PRIVATE SDL3::SDL3 user32)

    add_executable(aimouse_glfw_probe glfw_probe.cpp)
    target_link_libraries(aimouse_glfw_probe PRIVATE glfw user32 opengl32)

    foreach(probe aimouse_sdl_probe aimouse_glfw_probe)
        target_compile_features(${probe} PRIVATE cxx_std_20)
        target_compile_definitions(${probe} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(${probe} PRIVATE /W4 /utf-8 /permissive-)
        endif()
        add_custom_command(TARGET ${probe} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E
                $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${probe}>>,copy_if_different,true>
                $<TARGET_RUNTIME_DLLS:${probe}> "$<TARGET_FILE_DIR:${probe}>"
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()
