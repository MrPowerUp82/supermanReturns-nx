# Native Vulkan game renderer imported from superman_returns_recomp
# (docs/pcvk-import.md). Two libraries:
#   sr_pcvk_guest   PM4 mirror, guest-state capture and neutral render packets. No Vulkan.
#   sr_pcvk_vulkan  Vulkan recording of those packets (render targets, resolves,
#                   pipelines, descriptors, frontbuffer composition).
# The SPIR-V of the project-owned helper shaders is committed under
# graphics/vulkan/generated (tools/pcvk/regen_helper_shaders.py), so neither the
# Switch build nor the host tests need DXC.
cmake_minimum_required(VERSION 3.25)
set(SR_PCVK_VULKAN_INCLUDE "" CACHE PATH "Directory containing vulkan/vulkan.h (default: toolchain headers)")

add_library(sr_pcvk_guest STATIC
  native_renderer/pm4_mirror.cpp
  graphics/guest/captured_batch.cpp graphics/guest/constant_snapshot.cpp graphics/guest/draw_state.cpp
  graphics/guest/edram_alias.cpp graphics/guest/pm4_capture.cpp graphics/guest/primitive_expansion.cpp
  graphics/guest/render_packet.cpp graphics/guest/render_state.cpp graphics/guest/texture_capture.cpp
  graphics/guest/vertex_layout.cpp)
target_include_directories(sr_pcvk_guest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/native_renderer"
  "${CMAKE_CURRENT_SOURCE_DIR}/graphics/guest")
target_compile_features(sr_pcvk_guest PUBLIC cxx_std_23)

# Xenos texture decode (graphics/guest/texture_layout.cpp: DescribeTextureRanges, DecodeTextureLayout)
# needs the ReXGlue SDK (texture formats/info/conversion), so the game build adds it to
# sr_pcvk_guest (app/CMakeLists.txt); the SDK-free host tests provide a stand-in for
# DescribeTextureRanges (tests/pcvk/native/texture_stubs.cpp).

# Guest-thread capture (hooked D3D call -> WorkBatch -> RenderPacket worker). SDK-free except
# for the texture decoder it calls, which the final link (or a host test stub) provides.
add_library(sr_pcvk_frontend STATIC native_renderer/frontend.cpp native_renderer/shader_objects.cpp)
target_link_libraries(sr_pcvk_frontend PUBLIC sr_pcvk_guest)

add_library(sr_pcvk_vulkan STATIC
  graphics/shaders/shader_result.cpp graphics/shaders/shader_pack.cpp
  graphics/vulkan/spirv.cpp graphics/vulkan/loader.cpp graphics/vulkan/policy.cpp graphics/vulkan/context.cpp
  graphics/vulkan/device_requirements.cpp graphics/vulkan/descriptors.cpp graphics/vulkan/descriptor_sets.cpp
  graphics/vulkan/resources.cpp graphics/vulkan/image_state.cpp graphics/vulkan/render_targets.cpp
  graphics/vulkan/game_pipeline.cpp graphics/vulkan/game_renderer.cpp graphics/vulkan/depth_resolve.cpp
  graphics/vulkan/resolve.cpp graphics/vulkan/composition.cpp graphics/vulkan/game_frame.cpp)
target_link_libraries(sr_pcvk_vulkan PUBLIC sr_pcvk_guest)
target_include_directories(sr_pcvk_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/graphics/vulkan"
  "${CMAKE_CURRENT_SOURCE_DIR}/graphics/vulkan/generated")
if(SR_PCVK_VULKAN_INCLUDE)
  target_include_directories(sr_pcvk_vulkan SYSTEM PUBLIC "${SR_PCVK_VULKAN_INCLUDE}")
endif()
target_compile_definitions(sr_pcvk_vulkan PUBLIC VK_NO_PROTOTYPES)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Generic" AND NOT SWITCH)
  target_link_libraries(sr_pcvk_vulkan PRIVATE ${CMAKE_DL_LIBS})
endif()

# Host-only: the development shader service that shells out to Python/DXC.
add_library(sr_pcvk_shader_service STATIC graphics/shaders/vulkan_shader_service.cpp)
target_link_libraries(sr_pcvk_shader_service PUBLIC sr_pcvk_vulkan)
