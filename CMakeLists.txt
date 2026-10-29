cmake_minimum_required(VERSION 3.22)
project(powervr_vulkan_repros CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# glslc ships with the NDK (shader-tools/<host>/glslc); override with -DGLSLC=...
if(NOT GLSLC)
    file(GLOB GLSLC "${ANDROID_NDK}/shader-tools/*/glslc")
endif()
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found; pass -DGLSLC=/path/to/glslc")
endif()

set(SHADERS fullscreen.vert solid.frag no_output.frag heavy.frag buffer.comp image.comp)
set(SPIRV_INCLUDES)
foreach(shader ${SHADERS})
    set(out "${CMAKE_CURRENT_BINARY_DIR}/spirv/${shader}.inc")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/spirv"
        COMMAND ${GLSLC} --target-env=vulkan1.1 -mfmt=num -o "${out}"
                "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        COMMENT "glslc ${shader}")
    list(APPEND SPIRV_INCLUDES "${out}")
endforeach()
add_custom_target(spirv DEPENDS ${SPIRV_INCLUDES})

foreach(repro unused_attachment compute_cache concurrent_compile view_format)
    add_executable(repro_${repro} src/repro_${repro}.cpp)
    add_dependencies(repro_${repro} spirv)
    target_include_directories(repro_${repro} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/spirv")
    target_compile_options(repro_${repro} PRIVATE -Wall -Wextra -Wno-missing-field-initializers -O1 -g)
    target_link_libraries(repro_${repro} PRIVATE vulkan)
endforeach()
