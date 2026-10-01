// Draws with shaders translated offline by XenosRecomp: see pack_shaders.h.
//
// Everything that can make the draw fall back to Xenos is checked before the first
// command is recorded, so a fallback leaves no partial state behind. The Xenos path
// rebinds its pipeline, layout, descriptors and dynamic state after a pack draw
// (BindExternalGraphicsPipeline invalidates them).

#include <rex/graphics/vulkan/command_processor.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>

#include <rex/graphics/pack_shader_sources.h>
#include <rex/graphics/util/draw.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include "pack_shaders.h"

#if REX_PLATFORM_SWITCH
extern "C" void RexSwitchPerfCount(unsigned id);
extern "C" void RexSwitchPerfAdd(unsigned id, uint64_t value);
#endif

namespace rex::graphics::vulkan {

namespace {
// Switch profiler counters (switch_perf.cpp): pack draws, Xenos draws whose shaders are
// both in the pack, draws with a shader outside the pack.
constexpr unsigned kPerfPackDrawn = 33;
constexpr unsigned kPerfPackCoveredXenos = 34;
constexpr unsigned kPerfPackUncovered = 35;

constexpr uint32_t kDescriptorSetCount = 5;
constexpr uint32_t kDescriptorSetsPerPool = 1024;
constexpr size_t kUploadPageSize = size_t(16) << 20;
constexpr uint32_t kMaxVertexCount = 1u << 20;
constexpr uint32_t kMaxDetailLogs = 64;
}  // namespace

struct VulkanCommandProcessor::PackState {
  pack::Mode mode = pack::Mode::kOff;
  pack::Library library;

  // Draw mode only.
  bool draw_ready = false;
  uint32_t heap_size = pack::kHeapSize;
  VkDescriptorSetLayout set_layouts[kDescriptorSetCount] = {};
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_pool;
  std::deque<std::pair<uint64_t, VkDescriptorPool>> pools_used;
  std::vector<VkDescriptorPool> pools_free;
  VkDescriptorPool pool_current = VK_NULL_HANDLE;
  uint32_t pool_current_sets_left = 0;
  std::unordered_map<const pack::Entry*, VkShaderModule> modules;
  // (vertex shader entry, patched microcode hash) -> vertex input; empty attributes and
  // a reason when it can't be built.
  struct CachedVertexInput {
    pack::VertexInput input;
    const char* failure = nullptr;
  };
  std::unordered_map<uint64_t, CachedVertexInput> vertex_inputs;
  std::vector<uint32_t> indices;

  // Counters since startup, reported periodically.
  uint64_t draws = 0;
  uint64_t covered = 0;
  uint64_t drawn = 0;
  uint64_t pipelines_created = 0;
  uint64_t fallbacks[size_t(pack::Fallback::kCount)] = {};
  uint32_t details_logged[size_t(pack::Fallback::kCount)] = {};
  uint32_t pairs_logged = 0;
  std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
};

void VulkanCommandProcessor::PackStateDeleter::operator()(PackState* state) const {
  delete state;
}

void VulkanCommandProcessor::InitializePack() {
  pack_.reset();
  const pack::Mode mode = pack::ModeFromCvar();
  if (mode == pack::Mode::kOff) {
    return;
  }
  const std::vector<PackShaderSource>& sources = GetPackShaderSources();
  if (sources.empty()) {
    REXGPU_INFO("Shader pack: no pack registered by the app; draws use Xenos");
    return;
  }
  std::unique_ptr<PackState, PackStateDeleter> state(new PackState);
  state->mode = mode;
  state->library.Load(sources);
  if (!state->library.loaded()) {
    REXGPU_WARN("Shader pack: no usable shader in the pack; draws use Xenos");
    return;
  }
  if (mode != pack::Mode::kDraw) {
    REXGPU_INFO("Shader pack: identify mode, draws use Xenos");
    pack_ = std::move(state);
    return;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& properties = vulkan_device->properties();
  // The pack SPIR-V declares 64-bit push constants read through buffer device
  // addresses (unused with the uniform buffer specialization, but still declared) and
  // unbounded descriptor arrays.
  const char* missing = nullptr;
  if (!properties.shaderInt64) {
    missing = "shaderInt64";
  } else if (!properties.bufferDeviceAddress) {
    missing = "bufferDeviceAddress";
  } else if (!properties.runtimeDescriptorArray) {
    missing = "runtimeDescriptorArray";
  } else if (!properties.descriptorBindingPartiallyBound) {
    missing = "descriptorBindingPartiallyBound";
  } else if (!properties.scalarBlockLayout) {
    missing = "scalarBlockLayout";
  } else if (render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets) {
    missing = "host render targets";
  }
  if (missing) {
    REXGPU_WARN(
        "Shader pack: draw mode needs {} (vulkan_native_shader_features = true enables the "
        "features); identify mode only",
        missing);
    state->mode = pack::Mode::kIdentify;
    pack_ = std::move(state);
    return;
  }
  if (properties.maxPerStageDescriptorSampledImages < 3 * pack::kHeapSize ||
      properties.maxPerStageDescriptorSamplers < pack::kHeapSize) {
    state->heap_size = 16;
  }

  // Sets 0-2: Texture2D, Texture3D and TextureCube arrays; set 3: samplers; set 4: the
  // vertex, pixel and shared constants. Array element = texture fetch constant.
  const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkDescriptorBindingFlags partially_bound = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
  VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags = {};
  binding_flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  binding_flags.bindingCount = 1;
  binding_flags.pBindingFlags = &partially_bound;
  for (uint32_t i = 0; i < 4; ++i) {
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType =
        i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = state->heap_size;
    binding.stageFlags = stages;
    VkDescriptorSetLayoutCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.pNext = &binding_flags;
    create_info.bindingCount = 1;
    create_info.pBindings = &binding;
    if (dfn.vkCreateDescriptorSetLayout(device, &create_info, nullptr, &state->set_layouts[i]) !=
        VK_SUCCESS) {
      missing = "texture descriptor set layout";
      break;
    }
  }
  if (!missing) {
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = stages;
    }
    VkDescriptorSetLayoutCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    create_info.bindingCount = 3;
    create_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(device, &create_info, nullptr, &state->set_layouts[4]) !=
        VK_SUCCESS) {
      missing = "constants descriptor set layout";
    }
  }
  if (!missing) {
    // PushConstants of shader_common.h: three 64-bit addresses.
    VkPushConstantRange push_constants = {stages, 0, 3 * sizeof(uint64_t)};
    VkPipelineLayoutCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    create_info.setLayoutCount = kDescriptorSetCount;
    create_info.pSetLayouts = state->set_layouts;
    create_info.pushConstantRangeCount = 1;
    create_info.pPushConstantRanges = &push_constants;
    if (dfn.vkCreatePipelineLayout(device, &create_info, nullptr, &state->pipeline_layout) !=
        VK_SUCCESS) {
      missing = "pipeline layout";
    }
  }
  if (missing) {
    REXGPU_ERROR("Shader pack: failed to create the {}; identify mode only", missing);
    DestroyPackVulkanObjects(vulkan_device, *state);
    state->mode = pack::Mode::kIdentify;
    pack_ = std::move(state);
    return;
  }
  state->upload_pool = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      rex::align(kUploadPageSize, size_t(properties.minUniformBufferOffsetAlignment)));
  state->draw_ready = true;
  REXGPU_INFO("Shader pack: draw mode, {} texture slots per stage; unsupported draws use Xenos",
              state->heap_size);
  pack_ = std::move(state);
}

void VulkanCommandProcessor::ShutdownPack() {
  if (!pack_) {
    return;
  }
  DestroyPackVulkanObjects(GetVulkanDevice(), *pack_);
  pack_.reset();
}

void VulkanCommandProcessor::DestroyPackVulkanObjects(
    const ui::vulkan::VulkanDevice* vulkan_device, PackState& p) {
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (auto& module : p.modules) {
    if (module.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, module.second, nullptr);
    }
  }
  p.modules.clear();
  for (auto& used : p.pools_used) {
    dfn.vkDestroyDescriptorPool(device, used.second, nullptr);
  }
  p.pools_used.clear();
  for (VkDescriptorPool pool : p.pools_free) {
    dfn.vkDestroyDescriptorPool(device, pool, nullptr);
  }
  p.pools_free.clear();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, p.pool_current);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, p.pipeline_layout);
  for (VkDescriptorSetLayout& layout : p.set_layouts) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, layout);
  }
  p.upload_pool.reset();
  p.draw_ready = false;
}

void VulkanCommandProcessor::PackBeginFrame() {
  if (!pack_) {
    return;
  }
  PackState& p = *pack_;
  if (p.upload_pool) {
    p.upload_pool->Reclaim(frame_completed_);
  }
  if (!p.pools_used.empty()) {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    while (!p.pools_used.empty() && p.pools_used.front().first <= frame_completed_) {
      VkDescriptorPool pool = p.pools_used.front().second;
      vulkan_device->functions().vkResetDescriptorPool(vulkan_device->device(), pool, 0);
      p.pools_free.push_back(pool);
      p.pools_used.pop_front();
    }
  }

  const auto now = std::chrono::steady_clock::now();
  if (now - p.last_report < std::chrono::seconds(10)) {
    return;
  }
  p.last_report = now;
  const pack::StageStats& vs = p.library.stats(true);
  const pack::StageStats& ps = p.library.stats(false);
  std::string fallbacks;
  for (size_t i = 0; i < size_t(pack::Fallback::kCount); ++i) {
    if (p.fallbacks[i]) {
      fallbacks += fmt::format(" | {} {}", pack::FallbackName(pack::Fallback(i)), p.fallbacks[i]);
    }
  }
  REXGPU_INFO(
      "Shader pack ({}): VS {}/{} and PS {}/{} distinct microcode identified ({} ambiguous); "
      "draws {}: {} with both shaders in the pack, {} drawn with the pack, {} pipelines{}",
      p.mode == pack::Mode::kDraw ? "draw" : "identify", vs.identified, vs.distinct, ps.identified,
      ps.distinct, vs.ambiguous + ps.ambiguous, p.draws, p.covered, p.drawn, p.pipelines_created,
      fallbacks);
}

void VulkanCommandProcessor::PackEndSubmission() {
  if (pack_ && pack_->upload_pool) {
    pack_->upload_pool->FlushWrites();
  }
}

VulkanCommandProcessor::PackDrawResult VulkanCommandProcessor::IssuePackDraw(
    const PackDrawArguments& a) {
  if (!pack_) {
    return PackDrawResult::kXenos;
  }
  PackState& p = *pack_;
  const RegisterFile& regs = *register_file_;
  ++p.draws;

  const auto fallback = [&](pack::Fallback reason, const char* detail = nullptr) {
    ++p.fallbacks[size_t(reason)];
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(reason <= pack::Fallback::kPixelShaderUnknown ? kPerfPackUncovered
                                                                      : kPerfPackCoveredXenos);
#endif
    if (detail && p.details_logged[size_t(reason)] < 4) {
      ++p.details_logged[size_t(reason)];
      REXGPU_INFO("Shader pack: draw to Xenos ({}): {} (VS {:016X}, PS {:016X})",
                  pack::FallbackName(reason), detail, a.vertex_shader->ucode_data_hash(),
                  a.pixel_shader ? a.pixel_shader->ucode_data_hash() : 0);
    }
    return PackDrawResult::kXenos;
  };

  const pack::Entry* vs = p.library.Identify(
      true, a.vertex_shader->ucode_data_hash(),
      std::span<const uint32_t>(a.vertex_shader->ucode_dwords(),
                                a.vertex_shader->ucode_dword_count()));
  const pack::Entry* ps = nullptr;
  if (a.pixel_shader) {
    ps = p.library.Identify(false, a.pixel_shader->ucode_data_hash(),
                            std::span<const uint32_t>(a.pixel_shader->ucode_dwords(),
                                                      a.pixel_shader->ucode_dword_count()));
  }
  if (!vs) {
    return fallback(pack::Fallback::kVertexShaderUnknown);
  }
  if (a.pixel_shader && !ps) {
    return fallback(pack::Fallback::kPixelShaderUnknown);
  }
  ++p.covered;
  if (!p.draw_ready) {
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(kPerfPackCoveredXenos);
#endif
    return PackDrawResult::kXenos;
  }

  const PrimitiveProcessor::ProcessingResult& r = *a.primitive_processing_result;
  if (r.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
    return fallback(pack::Fallback::kHostVertexShaderType, "rectangle, point or tessellation");
  }
  if (a.memexport_used) {
    return fallback(pack::Fallback::kMemexport, "memexport");
  }
  switch (r.host_primitive_type) {
    case xenos::PrimitiveType::kLineList:
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kTriangleStrip:
      break;
    default:
      return fallback(pack::Fallback::kPrimitiveType,
                      "points, rectangles, quads and fans need Xenos geometry stages");
  }
  // The pack vertex shader only applies g_NdcScale / g_NdcOffset to XY.
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (vte.vtx_xy_fmt || vte.vtx_z_fmt || !vte.vtx_w0_fmt) {
    return fallback(pack::Fallback::kVertexTransform, "pre-divided position or 1/W");
  }
  if (a.vertex_shader_modification.vertex.user_clip_plane_count ||
      (a.vertex_shader->writes_point_size_edge_flag_kill_vertex() & 0b100)) {
    return fallback(pack::Fallback::kClipOrKill, "user clip planes or vertex kill");
  }

  // Render targets the pack pixel shader can write as is (Xenos converts the others in
  // its pixel shader).
  const VulkanRenderTargetCache::RenderPassKey render_pass_key =
      render_target_cache_->last_update_render_pass_key();
  {
    const xenos::ColorRenderTargetFormat formats[] = {
        render_pass_key.color_0_view_format, render_pass_key.color_1_view_format,
        render_pass_key.color_2_view_format, render_pass_key.color_3_view_format};
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(render_pass_key.depth_and_color_used & (1u << (1 + i)))) {
        continue;
      }
      switch (formats[i]) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
          break;
        default:
          return fallback(pack::Fallback::kRenderTarget,
                          "gamma, 7e3 or fixed -32...32 color format");
      }
      if (regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i])
              .color_exp_bias) {
        return fallback(pack::Fallback::kRenderTarget, "color exponent bias");
      }
    }
  }
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  if (ps && ((ps->ps_outputs & 0x10) || a.pixel_shader->writes_depth())) {
    return fallback(pack::Fallback::kPixelState, "pixel shader writes depth");
  }
  if (a.pixel_shader && color_control.alpha_to_mask_enable) {
    return fallback(pack::Fallback::kPixelState, "alpha to coverage");
  }

  // Textures: the views of the fetch constants the Xenos translation of the same
  // shader samples, with their samplers.
  struct TextureSlot {
    uint32_t set;
    uint32_t fetch_constant;
    VkImageView view;
  };
  TextureSlot textures[2 * pack::kHeapSize];
  uint32_t texture_count = 0;
  VkSampler samplers[pack::kHeapSize] = {};
  uint32_t sampler_mask = 0;
  if (!a.vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    return fallback(pack::Fallback::kTexture, "vertex shader textures");
  }
  if (a.pixel_shader) {
    const auto& xenos_samplers = a.pixel_shader->GetSamplerBindingsAfterTranslation();
    for (size_t i = 0; i < xenos_samplers.size() && i < current_samplers_pixel_.size(); ++i) {
      const uint32_t fetch_constant = xenos_samplers[i].fetch_constant;
      if (fetch_constant >= p.heap_size || (sampler_mask & (1u << fetch_constant))) {
        continue;
      }
      samplers[fetch_constant] = current_samplers_pixel_[i].second;
      sampler_mask |= 1u << fetch_constant;
    }
    for (const VulkanShader::TextureBinding& binding :
         a.pixel_shader->GetTextureBindingsAfterTranslation()) {
      const uint32_t fetch_constant = binding.fetch_constant;
      uint32_t set;
      switch (binding.dimension) {
        case xenos::FetchOpDimension::k2D:
          set = 0;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          set = 1;
          break;
        case xenos::FetchOpDimension::kCube:
          set = 2;
          break;
        default:
          return fallback(pack::Fallback::kTexture, "1D texture");
      }
      if (fetch_constant >= p.heap_size) {
        return fallback(pack::Fallback::kTexture, "texture fetch constant outside the heap");
      }
      bool sampler_in_pack = false;
      for (const pack::Sampler& sampler : ps->samplers) {
        sampler_in_pack |= sampler.reg == fetch_constant;
      }
      if (!sampler_in_pack) {
        return fallback(pack::Fallback::kTexture,
                        "fetch constant is not a sampler register of the pack shader");
      }
      if (!(sampler_mask & (1u << fetch_constant))) {
        return fallback(pack::Fallback::kSampler, "no sampler for the fetch constant");
      }
      // Signed and unsigned bindings of one fetch constant come as two bindings.
      bool duplicate = false;
      for (uint32_t i = 0; i < texture_count; ++i) {
        duplicate |= textures[i].fetch_constant == fetch_constant && textures[i].set == set;
      }
      if (duplicate) {
        continue;
      }
      // The pack shaders sample the view as is: per-component signedness, biasing,
      // gamma and exponent are applied by Xenos shaders only.
      const uint8_t signs = texture_cache_->GetActiveTextureSwizzledSigns(fetch_constant);
      bool is_signed;
      if (signs == uint8_t(xenos::TextureSign::kUnsigned) * 0b01010101) {
        is_signed = false;
      } else if (signs == uint8_t(xenos::TextureSign::kSigned) * 0b01010101) {
        is_signed = true;
      } else {
        return fallback(pack::Fallback::kTexture, "mixed, biased or gamma texture components");
      }
      if (regs.GetTextureFetch(fetch_constant).exp_adjust) {
        return fallback(pack::Fallback::kTexture, "texture exponent adjustment");
      }
      VkImageView view =
          texture_cache_->GetActiveBindingPackImageView(fetch_constant, binding.dimension, is_signed);
      if (view == VK_NULL_HANDLE) {
        return fallback(pack::Fallback::kTexture, "no texture of that dimension");
      }
      textures[texture_count++] = {set, fetch_constant, view};
    }
  }

  // Vertex input from the fetches Direct3D patched.
  const uint64_t vertex_input_key =
      a.vertex_shader->ucode_data_hash() ^ (uint64_t(uintptr_t(vs)) * 0x9E3779B97F4A7C15ull);
  auto vertex_input_it = p.vertex_inputs.find(vertex_input_key);
  if (vertex_input_it == p.vertex_inputs.end()) {
    PackState::CachedVertexInput cached;
    cached.failure = pack::BuildVertexInput(
        *vs,
        std::span<const uint32_t>(a.vertex_shader->ucode_dwords(),
                                  a.vertex_shader->ucode_dword_count()),
        cached.input);
    vertex_input_it = p.vertex_inputs.emplace(vertex_input_key, std::move(cached)).first;
  }
  if (vertex_input_it->second.failure) {
    return fallback(pack::Fallback::kVertexInput, vertex_input_it->second.failure);
  }
  const pack::VertexInput& input = vertex_input_it->second.input;

  // Indices, converted to host order (the Xenos vertex shader swaps them itself).
  const int32_t index_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  const uint32_t count = r.host_draw_vertex_count;
  bool indexed = false;
  bool index_32bit = false;
  uint32_t index_min = 0, index_max = count ? count - 1 : 0;
  switch (r.index_buffer_type) {
    case PrimitiveProcessor::ProcessedIndexBufferType::kNone:
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
      indexed = true;
      index_32bit = r.host_index_format == xenos::IndexFormat::kInt32;
      if (index_32bit && r.host_primitive_reset_enabled) {
        return fallback(pack::Fallback::kIndexBuffer, "32-bit indices with primitive reset");
      }
      const uint32_t index_size = index_32bit ? 4 : 2;
      if (uint64_t(r.guest_index_base) + uint64_t(count) * index_size > SharedMemory::kBufferSize) {
        return fallback(pack::Fallback::kIndexBuffer, "indices outside the memory");
      }
      const xenos::Endian endian = r.host_shader_index_endian;
      const uint32_t reset_index =
          regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx & 0xFFFF;
      p.indices.resize(count);
      index_min = UINT32_MAX;
      index_max = 0;
      const uint8_t* guest = memory_->TranslatePhysical<const uint8_t*>(r.guest_index_base);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t index;
        if (index_32bit) {
          uint32_t raw;
          std::memcpy(&raw, guest + size_t(i) * 4, 4);
          index = xenos::GpuSwap(raw, endian) & xenos::kVertexIndexMask;
        } else {
          uint16_t raw;
          std::memcpy(&raw, guest + size_t(i) * 2, 2);
          index = xenos::GpuSwap(raw, endian);
          if (r.host_primitive_reset_enabled && index == reset_index) {
            p.indices[i] = 0xFFFF;
            continue;
          }
        }
        p.indices[i] = index;
        index_min = std::min(index_min, index);
        index_max = std::max(index_max, index);
      }
      if (index_min > index_max) {
        // Only reset indices: nothing to draw.
        return PackDrawResult::kDrawn;
      }
      break;
    }
    default:
      return fallback(pack::Fallback::kIndexBuffer, "indices converted or generated by Xenos");
  }
  const int64_t first_vertex = int64_t(index_min) + index_offset;
  const int64_t last_vertex = int64_t(index_max) + index_offset;
  if (first_vertex < 0 || last_vertex - first_vertex + 1 > kMaxVertexCount) {
    return fallback(pack::Fallback::kVertexData, "vertex range");
  }
  const uint32_t vertex_count = uint32_t(last_vertex - first_vertex + 1);

  // Vertex streams: the vertices the draw uses, copied with the fetch constant's
  // endianness swapped.
  struct StreamUpload {
    const uint8_t* source;
    uint32_t bytes;
    xenos::Endian endian;
  };
  StreamUpload stream_uploads[16];
  for (size_t i = 0; i < input.streams.size(); ++i) {
    const pack::VertexStream& stream = input.streams[i];
    const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(stream.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex) {
      return fallback(pack::Fallback::kVertexData, "invalid vertex fetch constant");
    }
    const uint64_t buffer_bytes = uint64_t(fetch.size) * 4;
    const uint64_t start = uint64_t(stream.base) + uint64_t(first_vertex) * stream.stride;
    uint64_t bytes = uint64_t(vertex_count) * stream.stride;
    if (start >= buffer_bytes) {
      return fallback(pack::Fallback::kVertexData, "vertices past the end of their buffer");
    }
    // Robust buffer access reads zeros past a short buffer, like the guest's fetch.
    bytes = std::min(bytes, buffer_bytes - start);
    const uint64_t address = uint64_t(fetch.address) * 4 + start;
    if (address + bytes > SharedMemory::kBufferSize || bytes > kUploadPageSize / 2) {
      return fallback(pack::Fallback::kVertexData, "vertex data outside the memory or too big");
    }
    stream_uploads[i] = {memory_->TranslatePhysical<const uint8_t*>(uint32_t(address)),
                         uint32_t(bytes), fetch.endian};
  }

  // Shader modules and pipeline.
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const auto module_of = [&](const pack::Entry* entry) {
    auto it = p.modules.find(entry);
    if (it != p.modules.end()) {
      return it->second;
    }
    VkShaderModule module = ui::vulkan::util::CreateShaderModule(
        vulkan_device, entry->source->spirv.data(), entry->source->spirv.size_bytes());
    if (module == VK_NULL_HANDLE) {
      REXGPU_ERROR("Shader pack: failed to create the shader module of entry {}", entry->number);
    }
    p.modules.emplace(entry, module);
    return module;
  };
  VulkanPipelineCache::PackPipelineShaders shaders;
  shaders.vertex = module_of(vs);
  shaders.fragment = ps ? module_of(ps) : VK_NULL_HANDLE;
  if (shaders.vertex == VK_NULL_HANDLE || (ps && shaders.fragment == VK_NULL_HANDLE)) {
    return fallback(pack::Fallback::kResources, "shader module");
  }
  shaders.vertex_specialization = pack::kSpecConstantsUbo | input.vertex_spec;
  shaders.fragment_specialization = pack::kSpecConstantsUbo;
  if (ps && color_control.alpha_test_enable &&
      color_control.alpha_func != xenos::CompareFunction::kAlways) {
    shaders.fragment_specialization |=
        pack::kSpecAlphaTest | (uint32_t(color_control.alpha_func) << pack::kSpecAlphaFuncShift);
  }
  shaders.layout = p.pipeline_layout;
  VkVertexInputBindingDescription bindings[16];
  for (size_t i = 0; i < input.streams.size(); ++i) {
    bindings[i] = {uint32_t(i), input.streams[i].stride, VK_VERTEX_INPUT_RATE_VERTEX};
  }
  VkVertexInputAttributeDescription attributes[16];
  for (size_t i = 0; i < input.attributes.size() && i < 16; ++i) {
    const pack::VertexAttribute& attribute = input.attributes[i];
    attributes[i] = {attribute.location, attribute.binding, attribute.format, attribute.offset};
  }
  shaders.bindings = bindings;
  shaders.binding_count = uint32_t(input.streams.size());
  shaders.attributes = attributes;
  shaders.attribute_count = uint32_t(std::min<size_t>(input.attributes.size(), 16));
  {
    const uint64_t key_words[] = {uint64_t(uintptr_t(vs)), uint64_t(uintptr_t(ps)),
                                  (uint64_t(shaders.vertex_specialization) << 32) |
                                      shaders.fragment_specialization,
                                  input.hash};
    shaders.key = XXH3_64bits(key_words, sizeof(key_words));
  }
  VkPipeline pipeline;
  bool pipeline_created;
  if (!pipeline_cache_->ConfigurePackPipeline(
          a.vertex_shader_translation, a.pixel_shader_translation, r, a.normalized_depth_control,
          a.normalized_color_mask, render_pass_key, shaders, pipeline, pipeline_created)) {
    return fallback(pack::Fallback::kPipeline,
                    "state the pack pipeline can't express, or creation failed");
  }
  if (pipeline_created) {
    ++p.pipelines_created;
  }

  // Viewport: the same host viewport as the Xenos draw. The pack shader applies the
  // NDC transform to XY only, and DXC's -fvk-invert-y negates Y after it.
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(
      regs, texture_cache_->draw_resolution_scale_x(), texture_cache_->draw_resolution_scale_y(),
      false, device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1],
      true, a.normalized_depth_control, render_target_cache_->depth_float24_convert_in_pixel_shader(),
      true, a.pixel_shader && a.pixel_shader->writes_depth(), viewport_info);
  if (viewport_info.ndc_scale[2] != 1.0f || viewport_info.ndc_offset[2] != 0.0f) {
    return fallback(pack::Fallback::kVertexTransform, "OpenGL or reversed clip space depth");
  }

  // Uploads and descriptors. Past this point, a failure can't go back to Xenos cleanly
  // only if commands were recorded - none are until the draw itself.
  const size_t ubo_alignment = size_t(device_properties.minUniformBufferOffsetAlignment);
  VkDescriptorBufferInfo constant_buffers[3];
  {
    uint8_t* mapping;
    mapping = p.upload_pool->Request(frame_current_, pack::kFloatConstantsSize, ubo_alignment,
                                     constant_buffers[0].buffer, constant_buffers[0].offset);
    if (!mapping) {
      return fallback(pack::Fallback::kResources, "uniform upload");
    }
    std::memcpy(mapping, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], pack::kFloatConstantsSize);
    constant_buffers[0].range = pack::kFloatConstantsSize;
    mapping = p.upload_pool->Request(frame_current_, pack::kFloatConstantsSize, ubo_alignment,
                                     constant_buffers[1].buffer, constant_buffers[1].offset);
    if (!mapping) {
      return fallback(pack::Fallback::kResources, "uniform upload");
    }
    std::memcpy(mapping, &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X], pack::kFloatConstantsSize);
    constant_buffers[1].range = pack::kFloatConstantsSize;
    mapping = p.upload_pool->Request(frame_current_, pack::kSharedSize, ubo_alignment,
                                     constant_buffers[2].buffer, constant_buffers[2].offset);
    if (!mapping) {
      return fallback(pack::Fallback::kResources, "uniform upload");
    }
    constant_buffers[2].range = pack::kSharedSize;
    std::memset(mapping, 0, pack::kSharedSize);
    const auto put_u32 = [&](uint32_t offset, uint32_t value) {
      std::memcpy(mapping + offset, &value, sizeof(value));
    };
    const auto put_f32 = [&](uint32_t offset, float value) {
      std::memcpy(mapping + offset, &value, sizeof(value));
    };
    // Bindless indices: each heap is indexed by the texture fetch constant.
    for (uint32_t i = 0; i < 16; ++i) {
      put_u32(pack::kSharedTexture2DIndices + i * 4, i);
      put_u32(pack::kSharedTexture3DIndices + i * 4, i);
      put_u32(pack::kSharedTextureCubeIndices + i * 4, i);
      put_u32(pack::kSharedSamplerIndices + i * 4, i);
    }
    // XenosRecomp keeps 16 booleans per stage: b0-b15 for vertex shaders, b128-b143
    // (pixel shader b0-b15) in the high half.
    put_u32(pack::kSharedBooleans,
            (regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031] & 0xFFFF) |
                (regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_128_159] << 16));
    put_f32(pack::kSharedAlphaThreshold, regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF));
    put_u32(pack::kSharedAlphaFunction, uint32_t(color_control.alpha_func));
    put_f32(pack::kSharedNdcScale, viewport_info.ndc_scale[0]);
    put_f32(pack::kSharedNdcScale + 4, -viewport_info.ndc_scale[1]);
    put_f32(pack::kSharedNdcOffset, viewport_info.ndc_offset[0]);
    put_f32(pack::kSharedNdcOffset + 4, -viewport_info.ndc_offset[1]);
    for (uint32_t i = 0; i < 16; ++i) {
      put_u32(pack::kSharedInputRemap + i * 4, input.remap[i]);
    }
  }

  VkBuffer vertex_buffers[16];
  VkDeviceSize vertex_buffer_offsets[16];
  for (size_t i = 0; i < input.streams.size(); ++i) {
    const StreamUpload& upload = stream_uploads[i];
    const uint32_t words = (upload.bytes + 3) / 4;
    uint8_t* mapping = p.upload_pool->Request(frame_current_, size_t(words) * 4, 16,
                                              vertex_buffers[i], vertex_buffer_offsets[i]);
    if (!mapping) {
      return fallback(pack::Fallback::kResources, "vertex upload");
    }
    const uint8_t* source = upload.source;
    const xenos::Endian endian = upload.endian;
    for (uint32_t w = 0; w < words; ++w) {
      uint32_t value = 0;
      std::memcpy(&value, source + size_t(w) * 4, std::min<uint32_t>(4, upload.bytes - w * 4));
      value = xenos::GpuSwap(value, endian);
      std::memcpy(mapping + size_t(w) * 4, &value, 4);
    }
  }
  VkBuffer index_buffer = VK_NULL_HANDLE;
  VkDeviceSize index_buffer_offset = 0;
  if (indexed) {
    const size_t bytes = size_t(count) * (index_32bit ? 4 : 2);
    uint8_t* mapping =
        p.upload_pool->Request(frame_current_, bytes, 4, index_buffer, index_buffer_offset);
    if (!mapping) {
      return fallback(pack::Fallback::kResources, "index upload");
    }
    if (index_32bit) {
      std::memcpy(mapping, p.indices.data(), bytes);
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        const uint16_t value = uint16_t(p.indices[i]);
        std::memcpy(mapping + size_t(i) * 2, &value, 2);
      }
    }
  }

  if (p.pool_current_sets_left < kDescriptorSetCount) {
    if (p.pool_current != VK_NULL_HANDLE) {
      p.pools_used.emplace_back(frame_current_, p.pool_current);
      p.pool_current = VK_NULL_HANDLE;
    }
    if (!p.pools_free.empty()) {
      p.pool_current = p.pools_free.back();
      p.pools_free.pop_back();
    } else {
      const VkDescriptorPoolSize sizes[] = {
          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * p.heap_size * kDescriptorSetsPerPool / 5},
          {VK_DESCRIPTOR_TYPE_SAMPLER, p.heap_size * kDescriptorSetsPerPool / 5},
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * kDescriptorSetsPerPool / 5},
      };
      VkDescriptorPoolCreateInfo create_info = {};
      create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      create_info.maxSets = kDescriptorSetsPerPool;
      create_info.poolSizeCount = uint32_t(rex::countof(sizes));
      create_info.pPoolSizes = sizes;
      if (dfn.vkCreateDescriptorPool(device, &create_info, nullptr, &p.pool_current) !=
          VK_SUCCESS) {
        p.pool_current = VK_NULL_HANDLE;
        p.pool_current_sets_left = 0;
        return fallback(pack::Fallback::kResources, "descriptor pool");
      }
    }
    p.pool_current_sets_left = kDescriptorSetsPerPool / kDescriptorSetCount * kDescriptorSetCount;
  }
  VkDescriptorSet sets[kDescriptorSetCount];
  {
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = p.pool_current;
    allocate_info.descriptorSetCount = kDescriptorSetCount;
    allocate_info.pSetLayouts = p.set_layouts;
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, sets) != VK_SUCCESS) {
      p.pool_current_sets_left = 0;
      return fallback(pack::Fallback::kResources, "descriptor sets");
    }
    p.pool_current_sets_left -= kDescriptorSetCount;
  }
  {
    VkDescriptorImageInfo image_infos[2 * pack::kHeapSize + pack::kHeapSize];
    VkWriteDescriptorSet writes[2 * pack::kHeapSize + pack::kHeapSize + 1];
    uint32_t write_count = 0;
    uint32_t image_info_count = 0;
    for (uint32_t i = 0; i < texture_count; ++i) {
      VkDescriptorImageInfo& info = image_infos[image_info_count++];
      info.sampler = VK_NULL_HANDLE;
      info.imageView = textures[i].view;
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet& write = writes[write_count++];
      write = {};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = sets[textures[i].set];
      write.dstBinding = 0;
      write.dstArrayElement = textures[i].fetch_constant;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &info;
    }
    uint32_t sampler_bits = sampler_mask;
    uint32_t fetch_constant;
    while (rex::bit_scan_forward(sampler_bits, &fetch_constant)) {
      sampler_bits &= ~(1u << fetch_constant);
      VkDescriptorImageInfo& info = image_infos[image_info_count++];
      info.sampler = samplers[fetch_constant];
      info.imageView = VK_NULL_HANDLE;
      info.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VkWriteDescriptorSet& write = writes[write_count++];
      write = {};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = sets[3];
      write.dstBinding = 0;
      write.dstArrayElement = fetch_constant;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      write.pImageInfo = &info;
    }
    VkWriteDescriptorSet& write = writes[write_count++];
    write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets[4];
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 3;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = constant_buffers;
    dfn.vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
  }

  if (p.pairs_logged < kMaxDetailLogs && pipeline_created) {
    ++p.pairs_logged;
    REXGPU_INFO(
        "Shader pack: drawing VS entry {} + PS entry {} ({} attributes in {} streams, {} "
        "textures, topology {}, {} {} vertices {}..{})",
        vs->number, ps ? int64_t(ps->number) : int64_t(-1), input.attributes.size(),
        input.streams.size(), texture_count, uint32_t(r.host_primitive_type),
        indexed ? "indexed" : "auto", count, first_vertex, last_vertex);
  }

  // Record.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(render_target_cache_->last_update_render_pass(),
                                                    render_target_cache_->last_update_framebuffer());
  BindExternalGraphicsPipeline(pipeline);
  UpdateDynamicState(viewport_info, a.primitive_polygonal, a.normalized_depth_control);
  deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                   p.pipeline_layout, 0, kDescriptorSetCount, sets,
                                                   0, nullptr);
  const uint64_t push_constants[3] = {};
  deferred_command_buffer_.CmdVkPushConstants(
      p.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
      sizeof(push_constants), push_constants);
  if (!input.streams.empty()) {
    deferred_command_buffer_.CmdVkBindVertexBuffers(0, uint32_t(input.streams.size()),
                                                    vertex_buffers, vertex_buffer_offsets);
  }
#if REX_PLATFORM_SWITCH
  RexSwitchPerfCount(5);
  RexSwitchPerfAdd(6, uint64_t(dynamic_scissor_.extent.width) * dynamic_scissor_.extent.height);
  RexSwitchPerfCount(kPerfPackDrawn);
#endif
  if (indexed) {
    deferred_command_buffer_.CmdVkBindIndexBuffer(
        index_buffer, index_buffer_offset, index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    // Index i reads vertex i + VGT_INDX_OFFSET, and the upload starts at first_vertex.
    deferred_command_buffer_.CmdVkDrawIndexed(count, 1, 0, int32_t(index_offset - first_vertex),
                                              0);
  } else {
    deferred_command_buffer_.CmdVkDraw(count, 1, 0, 0);
  }
  ++p.drawn;
  return PackDrawResult::kDrawn;
}

}  // namespace rex::graphics::vulkan
