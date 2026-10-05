# Host tests of the code imported from superman_returns_recomp (app/src/pcvk).
# They need neither the game nor the ReXGlue SDK and run against any Vulkan
# driver (lavapipe is enough):
#   cmake -S tests/pcvk -B out/tests-pcvk -G Ninja -DSR_VULKAN_DXC=<dxc>
#   cmake --build out/tests-pcvk && ctest --test-dir out/tests-pcvk --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(sr_pcvk_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()
find_package(Threads REQUIRED)
set(SR_PCVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/pcvk")
add_subdirectory("${SR_PCVK_DIR}" pcvk)

add_executable(sr_pcvk_native_tests
  native/main.cpp native/test_checked_guest_memory.cpp native/test_shader_requirements.cpp
  native/test_guest_state.cpp native/test_render_packets.cpp native/test_render_state.cpp
  native/test_vertex_layout.cpp native/test_primitive_expansion.cpp native/test_texture.cpp
  native/test_pm4_mirror.cpp native/test_game_profile.cpp
  native/test_index_endian.cpp native/test_shaders.cpp native/test_frontend.cpp
  native/texture_stubs.cpp)
target_include_directories(sr_pcvk_native_tests PRIVATE native "${SR_PCVK_DIR}/native_renderer" "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src" "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/include")
target_link_libraries(sr_pcvk_native_tests PRIVATE sr_pcvk_frontend Threads::Threads)
add_test(NAME sr_pcvk_native_tests COMMAND sr_pcvk_native_tests)

add_executable(sr_pcvk_vulkan_tests
  vulkan/main.cpp vulkan/test_context.cpp vulkan/test_descriptors.cpp
  vulkan/test_device_requirements.cpp vulkan/test_game_pipeline.cpp vulkan/test_image_state.cpp
  vulkan/test_render_targets.cpp vulkan/test_resolve.cpp vulkan/test_resources.cpp
  vulkan/test_shader_service.cpp vulkan/test_composition.cpp)
target_include_directories(sr_pcvk_vulkan_tests PRIVATE native "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src")
target_link_libraries(sr_pcvk_vulkan_tests PRIVATE sr_pcvk_vulkan sr_pcvk_shader_service)
add_test(NAME sr_pcvk_vulkan_tests COMMAND sr_pcvk_vulkan_tests)

# GPU fixtures: real recording through the production renderer on whatever Vulkan device is
# present (lavapipe works), with readback of the pixels. Need DXC for the test HLSL.
find_program(SR_PCVK_DXC dxc HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.tools/dxc/bin")
if(SR_PCVK_DXC)
  set(gpu_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/gpu-shaders")
  file(MAKE_DIRECTORY "${gpu_shader_dir}")
  set(gpu_spv "")
  foreach(stage IN ITEMS vs ps)
    if(stage STREQUAL "vs")
      set(entry VSMain)
    else()
      set(entry PSMain)
    endif()
    add_custom_command(OUTPUT "${gpu_shader_dir}/${stage}.spv"
      COMMAND "${SR_PCVK_DXC}" -spirv -fspv-target-env=vulkan1.1 -T ${stage}_6_0 -E ${entry} -Fo "${gpu_shader_dir}/${stage}.spv" "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shaders/target_tests.hlsl"
      DEPENDS gpu/shaders/target_tests.hlsl VERBATIM)
    list(APPEND gpu_spv "${gpu_shader_dir}/${stage}.spv")
  endforeach()
  add_custom_command(OUTPUT "${gpu_shader_dir}/resolve_sample.spv"
    COMMAND "${SR_PCVK_DXC}" -spirv -fspv-target-env=vulkan1.1 -T ps_6_0 -E PSMain -Fo "${gpu_shader_dir}/resolve_sample.spv" "${CMAKE_CURRENT_SOURCE_DIR}/gpu/shaders/resolve_sample.hlsl"
    DEPENDS gpu/shaders/resolve_sample.hlsl VERBATIM)
  list(APPEND gpu_spv "${gpu_shader_dir}/resolve_sample.spv")
  add_executable(sr_pcvk_gpu_fixtures gpu/gpu_fixtures.cpp ${gpu_spv})
  target_link_libraries(sr_pcvk_gpu_fixtures PRIVATE sr_pcvk_vulkan)
  target_compile_definitions(sr_pcvk_gpu_fixtures PRIVATE SR_TARGET_SHADER_DIR="${gpu_shader_dir}")
  foreach(mode IN ITEMS "" --targets --pipeline-cache --target-draws --game-record --game-record-merged
                        --resolve-record --depth-resolve --edram-alias --alias-defaults --resolve-sample
                        --game-frame --stacked-resolve --composition --resolve-copy)
    string(REPLACE "--" "" name "gpu_fixture${mode}")
    add_test(NAME ${name} COMMAND sr_pcvk_gpu_fixtures ${mode})
  endforeach()
endif()
