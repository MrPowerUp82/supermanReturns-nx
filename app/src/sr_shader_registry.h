#pragma once
#include "sr_shader_library.h"
#include <mutex>
#include <unordered_map>

namespace sr::native {
struct ShaderIdentity;
// Loaded before guest threads start, then immutable for the process lifetime.
// Engine resource addresses are not D3D shader addresses or pipeline handles.
class ShaderRegistry {
 public:
  void Load(const std::filesystem::path& path);
  const Shader* Identify(std::span<const uint8_t> buffer) const;
  void RememberEngineResource(uint32_t resource, const Shader* shader);
  const Shader* FindEngineResource(uint32_t resource) const;
  void RememberD3DObject(uint32_t object, const ShaderIdentity& identity);
  const Shader* FindD3DObject(uint32_t object) const;
  void ForgetD3DObject(uint32_t object);
  size_t size() const { return library_.shaders().size(); }
  const std::vector<Shader>& shaders() const { return library_.shaders(); }
 private:
  BibliotecaShaders library_;
  bool loaded_ = false;
  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, const Shader*> resources_;
  std::unordered_map<uint32_t, const Shader*> d3d_objects_;
};
ShaderRegistry& RuntimeShaders();
void InitializeRuntimeShaders();
}  // namespace sr::native
