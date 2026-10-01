#pragma once
#include "sr_shader_library.h"
#include <mutex>
#include <unordered_map>

namespace sr::native {
// Loaded before guest threads start, then immutable for the process lifetime.
// Engine resource addresses are not D3D shader addresses or pipeline handles.
class ShaderRegistry {
 public:
  void Load(const std::filesystem::path& path);
  const Shader* Identify(std::span<const uint8_t> buffer) const;
  void RememberEngineResource(uint32_t resource, const Shader* shader);
  const Shader* FindEngineResource(uint32_t resource) const;
  size_t size() const { return library_.shaders().size(); }
 private:
  BibliotecaShaders library_;
  bool loaded_ = false;
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, const Shader*> resources_;
};
ShaderRegistry& RuntimeShaders();
void InitializeRuntimeShaders();
}  // namespace sr::native
