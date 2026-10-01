#include "sr_shader_registry.h"
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Expected local shader library path");
    sr::native::BibliotecaShaders library;
    library.Cargar(argv[1]);
    sr::native::ShaderRegistry registry;
    Check(!registry.Identify({}), "Empty unloaded registry must reject input");
    registry.Load(argv[1]);
    Check(registry.size() == library.shaders().size(), "Entry count mismatch");
    for (const auto& entry : library.shaders()) {
      const auto* shader = registry.Identify(entry.original);
      Check(shader && shader->huella == entry.huella && shader->vertices == entry.vertices,
            "Original container not identified");
      auto padded = entry.original;
      padded.insert(padded.end(), 16, 0xCC);
      Check(registry.Identify(padded) == shader, "Trailing resource padding must be allowed");
      Check(!registry.Identify(std::span(entry.original).first(entry.original.size() - 1)),
            "Truncated container must be rejected");
      padded[0] ^= 1;
      Check(!registry.Identify(padded), "Unknown container must be rejected");
      registry.RememberEngineResource(0x81234560, shader);
      Check(registry.FindEngineResource(0x81234560) == shader, "Resource association failed");
      registry.RememberEngineResource(0x81234560, nullptr);
      Check(!registry.FindEngineResource(0x81234560), "Reused address retained stale shader");
      registry.RememberEngineResource(0, shader);
      Check(!registry.FindEngineResource(0), "Null resource must not be registered");
    }
    bool refused = false;
    try { registry.Load(argv[1]); } catch (const std::logic_error&) { refused = true; }
    Check(refused, "Reload must not invalidate registered shader pointers");
    std::cout << "Registry: " << registry.size()
              << " containers identified; padding, truncation, address reuse and reload checks passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
