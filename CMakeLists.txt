cmake_minimum_required(VERSION 3.20)
project(CustomRayTracingEngine LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CustomRayTracingEngine requires Windows, Direct3D 12, and DXR.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(ENGINE_ENABLE_NRD "Build NVIDIA NRD and NRI for path-tracing denoising" ON)
if(ENGINE_ENABLE_NRD)
    # NVIDIA's current NRD/NRI SDKs require CMake 3.30+. Limit NRI to the D3D12
    # backend used by this engine and avoid fetching unrelated vendor SDKs.
    set(NRI_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_D3D12_SUPPORT ON CACHE BOOL "" FORCE)
    set(NRI_ENABLE_VK_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_WGPU_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_METAL_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NONE_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_VALIDATION_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_NVAPI OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_AMDAGS OFF CACHE BOOL "" FORCE)
    set(NRI_ENABLE_AGILITY_SDK_SUPPORT OFF CACHE BOOL "" FORCE)
    set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
    set(NRD_NRI ON CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXIL_SHADERS ON CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
    # The 10:10:10:2 encoding is NRD's recommended format and carries the
    # material-ID payload used by its temporal shader variants.
    set(NRD_NORMAL_ENCODING "2" CACHE STRING "" FORCE)
    set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "" FORCE)
    set(NRD_SUPPORTS_QUAD_INTRINSICS OFF CACHE BOOL "" FORCE)
    set(NRD_SHADERS_PATH "${CMAKE_CURRENT_BINARY_DIR}/nrd_shaders" CACHE STRING "" FORCE)

    add_subdirectory(vendor/nvidia/NRI "${CMAKE_CURRENT_BINARY_DIR}/nri")
    add_subdirectory(vendor/nvidia/NRD "${CMAKE_CURRENT_BINARY_DIR}/nrd")
    # Recent Windows SDKs expose the opacity-micromap structs but omit the
    # alignment macro NRI uses to detect them, causing duplicate fallback types.
    target_compile_definitions(NRI_D3D12 PRIVATE D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT=128)
endif()

set(ASSIMP_ROOT "C:/msys64/mingw64" CACHE PATH "Root directory of the Assimp installation")
find_path(ASSIMP_INCLUDE_DIR NAMES assimp/cimport.h HINTS "${ASSIMP_ROOT}/include" REQUIRED)
find_library(ASSIMP_LIBRARY NAMES assimp libassimp.dll.a HINTS "${ASSIMP_ROOT}/lib" REQUIRED)
set(ASSIMP_RUNTIME_DLLS
    "libassimp-6.dll;libgcc_s_seh-1.dll;libstdc++-6.dll;libwinpthread-1.dll;libminizip-1.dll;libbz2-1.dll;zlib1.dll"
    CACHE STRING "Assimp and MinGW runtime DLLs to stage beside CustomEngine.exe")

set(JOLT_ROOT "C:/msys64/ucrt64" CACHE PATH "Root directory of the MSYS2 UCRT64 Jolt installation")
find_program(JOLT_GXX_EXECUTABLE NAMES g++.exe HINTS "${JOLT_ROOT}/bin" REQUIRED)
set(JOLT_IMPORT_LIBRARY "${JOLT_ROOT}/lib/libJolt.dll.a")
set(JOLT_BRIDGE_DLL "${CMAKE_CURRENT_BINARY_DIR}/JoltBridge.dll")
if(NOT EXISTS "${JOLT_ROOT}/include/Jolt/Jolt.h" OR NOT EXISTS "${JOLT_IMPORT_LIBRARY}")
    message(FATAL_ERROR "MSYS2 UCRT64 Jolt headers/import library were not found under ${JOLT_ROOT}")
endif()
add_custom_command(
    OUTPUT "${JOLT_BRIDGE_DLL}"
    COMMAND "${JOLT_GXX_EXECUTABLE}" -std=c++20 -O2 -shared -DNDEBUG
        -DJPH_SHARED_LIBRARY -DJPH_CROSS_PLATFORM_DETERMINISTIC -DJPH_OBJECT_LAYER_BITS=16
        -DJPH_DEBUG_RENDERER -DJPH_PROFILE_ENABLED -DJPH_OBJECT_STREAM
        -DJPH_USE_AVX2 -DJPH_USE_AVX -DJPH_USE_SSE4_1 -DJPH_USE_SSE4_2
        -DJPH_USE_LZCNT -DJPH_USE_TZCNT -DJPH_USE_F16C
        -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfpmath=sse
        "-I${JOLT_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tools/jolt_bridge.cpp"
        "${JOLT_IMPORT_LIBRARY}" -o "${JOLT_BRIDGE_DLL}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/jolt_bridge.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/OceanWave.h"
    COMMENT "Building the isolated C ABI bridge to MSYS2 Jolt 5.3"
    VERBATIM
)
add_custom_target(JoltBridgeBuild ALL DEPENDS "${JOLT_BRIDGE_DLL}")

set(_WINDOWS_KIT_DXC_HINTS)
file(GLOB _WINDOWS_KIT_DXC_HINTS
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
    "$ENV{WindowsSdkDir}/bin/*/x64")
find_program(DXC_EXECUTABLE NAMES dxc.exe dxc
    HINTS ${_WINDOWS_KIT_DXC_HINTS} "$ENV{VULKAN_SDK}/Bin")
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC was not found. Install the Windows SDK DirectX Shader Compiler (dxc.exe).")
endif()

set(GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RAY_TRACING_DXIL "${GENERATED_SHADER_DIR}/RayTracingPBR.dxil")
set(RAY_TRACING_BYTECODE_HEADER "${GENERATED_SHADER_DIR}/RayTracingPBR_Bytecode.h")
set(RAY_TRACING_DXC_NRD_ARGS -D ENGINE_ENABLE_NRD=0)
if(ENGINE_ENABLE_NRD)
    set(RAY_TRACING_DXC_NRD_ARGS
        -D ENGINE_ENABLE_NRD=1
        -I "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvidia/NRD/Shaders")
    file(GLOB_RECURSE NRD_HLSL_INCLUDE_DEPENDENCIES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvidia/NRD/Shaders/*.hlsli")
endif()
add_custom_command(
    OUTPUT "${RAY_TRACING_BYTECODE_HEADER}"
    BYPRODUCTS "${RAY_TRACING_DXIL}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_SHADER_DIR}"
    COMMAND "${DXC_EXECUTABLE}" -T lib_6_5 -O3
        ${RAY_TRACING_DXC_NRD_ARGS}
        -Fo "${RAY_TRACING_DXIL}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/RayTracingPBR.hlsl"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_dxil.py" "${RAY_TRACING_DXIL}" "${RAY_TRACING_BYTECODE_HEADER}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/RayTracingPBR.hlsl"
        ${NRD_HLSL_INCLUDE_DEPENDENCIES}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_dxil.py"
    COMMENT "Compiling and embedding the DXR shader library"
    VERBATIM
)

if(ENGINE_ENABLE_NRD)
    set(NRD_COMPOSITE_DXIL "${GENERATED_SHADER_DIR}/NrdComposite.dxil")
    set(NRD_COMPOSITE_BYTECODE_HEADER "${GENERATED_SHADER_DIR}/NrdComposite_Bytecode.h")
    add_custom_command(
        OUTPUT "${NRD_COMPOSITE_BYTECODE_HEADER}"
        BYPRODUCTS "${NRD_COMPOSITE_DXIL}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_SHADER_DIR}"
        COMMAND "${DXC_EXECUTABLE}" -T cs_6_5 -E main -O3
            -I "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nvidia/NRD/Shaders"
            -Fo "${NRD_COMPOSITE_DXIL}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/NrdComposite.hlsl"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_dxil.py"
            "${NRD_COMPOSITE_DXIL}" "${NRD_COMPOSITE_BYTECODE_HEADER}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/NrdComposite.hlsl"
            ${NRD_HLSL_INCLUDE_DEPENDENCIES}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_dxil.py"
        COMMENT "Compiling the NRD output composition shader"
        VERBATIM
    )
endif()

set(ENGINE_SOURCES
    src/main.cpp
    src/core/Window.cpp
    src/ecs/Scene.cpp
    src/physics/JoltPhysicsWorld.cpp
    src/rhi/D3D12Device.cpp
    src/rhi/AccelerationStructure.cpp
    src/rhi/RayTracingPipeline.cpp
    src/rhi/NRDDenoiser.cpp
    src/editor/ImGuiRenderer.cpp
    src/editor/EditorUI.cpp
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/imgui_impl_win32.cpp
)

set(ENGINE_HEADERS
    src/core/Math.h
    src/core/Timer.h
    src/core/Window.h
    src/ecs/Entity.h
    src/ecs/Components.h
    src/ecs/Scene.h
    src/physics/JoltPhysicsWorld.h
    src/rhi/D3D12Device.h
    src/rhi/AccelerationStructure.h
    src/rhi/RayTracingPipeline.h
    src/rhi/NRDDenoiser.h
    src/rhi/ConstantBuffers.h
    src/editor/ImGuiRenderer.h
    src/editor/EditorUI.h
    "${RAY_TRACING_BYTECODE_HEADER}"
)
if(ENGINE_ENABLE_NRD)
    list(APPEND ENGINE_HEADERS "${NRD_COMPOSITE_BYTECODE_HEADER}")
endif()

add_executable(CustomEngine WIN32 ${ENGINE_SOURCES} ${ENGINE_HEADERS})
add_dependencies(CustomEngine JoltBridgeBuild)
target_include_directories(CustomEngine PRIVATE
    "${GENERATED_SHADER_DIR}"
    src
    shaders
    vendor/imgui
    vendor/cgltf
    vendor/stb
    vendor/nvidia/NRI/Include
    vendor/nvidia/NRD/Include
    vendor/nvidia/NRD/Integration
    "${ASSIMP_INCLUDE_DIR}"
)
target_compile_definitions(CustomEngine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(CustomEngine PRIVATE
    d3d12.lib
    dxgi.lib
    dxguid.lib
    user32.lib
    gdi32.lib
    shell32.lib
    "${ASSIMP_LIBRARY}"
)
if(ENGINE_ENABLE_NRD)
    target_link_libraries(CustomEngine PRIVATE NRI NRD)
    target_compile_definitions(CustomEngine PRIVATE ENGINE_ENABLE_NRD=1)
else()
    target_compile_definitions(CustomEngine PRIVATE ENGINE_ENABLE_NRD=0)
endif()

if(MSVC)
    target_compile_options(CustomEngine PRIVATE /W3 /EHsc /MP /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(EngineMathTests tests/MathTests.cpp)
    target_include_directories(EngineMathTests PRIVATE src)
    target_compile_definitions(EngineMathTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_compile_options(EngineMathTests PRIVATE /W3 /EHsc /utf-8)
    endif()
    add_test(NAME EngineMathChecks COMMAND EngineMathTests)
endif()

add_custom_command(TARGET CustomEngine POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:CustomEngine>/assets"
    VERBATIM
)
add_custom_command(TARGET CustomEngine POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
        "-DENGINE_EXE=$<TARGET_FILE:CustomEngine>"
        "-DOUTPUT_DIR=$<TARGET_FILE_DIR:CustomEngine>"
        "-DASSIMP_RUNTIME_DIR=${ASSIMP_ROOT}/bin"
        "-DASSIMP_RUNTIME_DLLS=${ASSIMP_RUNTIME_DLLS}"
        "-DJOLT_BRIDGE=${JOLT_BRIDGE_DLL}"
        "-DJOLT_RUNTIME_DIR=${JOLT_ROOT}/bin"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_runtime_dependencies.cmake"
    VERBATIM
)
add_custom_command(TARGET CustomEngine POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:CustomEngine>"
        "$<TARGET_FILE_DIR:CustomEngine>/WaterDemo.exe"
    VERBATIM
)
add_custom_target(WaterDemo
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:CustomEngine>"
        "$<TARGET_FILE_DIR:CustomEngine>/WaterDemo.exe"
    DEPENDS CustomEngine
    COMMENT "Staging the interactive water demo executable"
    VERBATIM
)
