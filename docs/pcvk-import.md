# Renderer Vulkan do projeto PC (pcvk)

Origem: [MrPowerUp82/superman_returns_recomp](https://github.com/MrPowerUp82/superman_returns_recomp),
commit `f5ac13b3aa6623cc022c59624c978cd2b5280bbf` (renderer `native` + backend Vulkan "M3",
chegou à gameplay no PC). O código vive em `app/src/pcvk/` com a mesma árvore e os mesmos
namespaces do original, para que correções possam ser levadas nos dois sentidos com `diff`.

## O que foi importado

| Diretório em `app/src/pcvk/` | Papel | Origem no PC |
|---|---|---|
| `graphics/guest/` | pacotes de render neutros: decodificação do PM4, estado de draw, superfícies, layout de vértices/texturas, expansão de primitivas | `port/src/graphics/guest/` |
| `graphics/vulkan/` | gravação Vulkan: alvos de render, resolves, alias EDRAM, pipelines, descriptors, composição do frontbuffer, `GameFrame` | `port/src/graphics/vulkan/` |
| `graphics/shaders/` | contrato de binding `sr-vulkan-buffers-v1`, decodificador do resultado de shader, **pack offline** (novo) | `port/src/graphics/shaders/` |
| `native_renderer/pm4_mirror.*`, `game_profile.h`, `xenos_tiling.h`, ... | espelho do estado PM4 e perfil D3D confirmado do XDK 2.0.3529 | `port/src/native_renderer/` |
| `native_renderer/frontend.*` (novo) | captura nas threads do jogo: chamada D3D → bytes do anel PM4 + estado do device + vértices/índices/texturas → worker → `RenderPacket` | recorte da parte "front end" de `native_renderer.cpp` |
| `native_renderer/shader_objects.*` (novo) | objeto de shader do jogo → container original capturado na criação | `shader_registry.cpp`, simplificado |

Não foram importados: o renderer D3D12 (`native_renderer.cpp`, ~6 600 linhas), o provider/presenter
Win32, o `ImmediateRenderer` da UI (o SDK desenha a UI), o serviço de shaders em execução
(Python + DXC), o launcher e os pós-efeitos D3D12.

## Mudanças em relação ao PC

- `Context::OpenOffscreenDevice` (sem superfície) e `Context::Adopt`: o renderer usa o
  `VkDevice` do SDK sem ser dono dele. `Context` não destrói nada que adotou.
- Pontos de entrada de apresentação (`vkCreateSwapchainKHR`, surfaces, debug utils) são opcionais no
  carregador; no Switch `Loader::Open` nunca usa `dlopen`.
- `QueueLock`: `GameFrame` pega o mutex da fila do SDK em vez de um `std::mutex` próprio.
- Shaders: `ShaderPoll::unavailable` (container fora do pack) pula o draw e conta; com
  `skip_failed_draws` um draw que o renderer não consegue expressar (formato de textura sem suporte,
  pipeline recusado) custa o draw, não a sessão. Perda de device e falta de memória continuam fatais.
- `shader_result.cpp` (decodificador) separado de `vulkan_shader_service.cpp` (serviço Python, só host).
- SPIR-V dos shaders auxiliares (composição, resolve de profundidade, alias EDRAM) fica commitado em
  `graphics/vulkan/generated/` — o build do Switch não tem DXC. Regenerar/verificar:
  `python tools/pcvk/regen_helper_shaders.py --dxc <dxc> [--check]`.
- Front end sem *write watch* (o Horizon não entrega os page faults): texturas até 4 MB são
  revalidadas por hash a cada quadro, maiores a cada 30; buffers só por `Unlock` e, se pequenos, por hash.
  Os bytes de uma textura só acompanham o comando que os carrega (captura "leve" nas repetições).
- Todo acesso à memória do jogo passa por `GuestAccess`: páginas não comprometidas viram um comando
  descartado e contado (`capture_failures`), nunca uma leitura errada.
- Worker do front end numa thread de 8 MB (o decodificador usa dezenas de KB de pilha e o compilador
  do driver recursa; a pilha padrão do Horizon é 128 KB).

## Como o dado flui

```
chamada D3D do jogo ──hook (sr_vk_hooks.cpp)──► Frontend (thread do jogo)
   original roda primeiro                          copia: PM4 do segmento, estado do device,
                                                   vértices/índices/texturas, shaders
                                                          │ lote ≤ 32 comandos / 4 MB
                                                          ▼
                                          worker: espelho PM4 → DecodeRenderPacket
                                                          │
                                       Presentation::Submit (sr_vk_present.cpp)
                                                          ▼
                       GameFrame::Enqueue ── (no swap) ──► grava o quadro, submete (mutex da fila do SDK)
                                                          ▼
                       compõe o frontbuffer (gama, 16:9) na imagem de saída do presenter do SDK
```

O consumidor PM4 (`sr_native_ring.cpp`, `instant_gpu`) só cumpre o contrato de sincronização do
jogo (fences, write-back, interrupções, contador de quadros): a GPU é tratada como infinitamente
rápida, como no sistema gráfico do PC. Os draws do PM4 são consumidos sem efeito.

## Testes

```sh
sudo apt-get install -y cmake ninja-build clang libvulkan-dev mesa-vulkan-drivers libxxhash-dev
tests/test_pcvk.sh          # unitários + fixtures de GPU (lavapipe) + contrato do ABI + ponta a ponta
```

- 78 testes de unidade do guest/front end, 44 do Vulkan, 15 fixtures de GPU (alvos, resolves,
  alias EDRAM, composição…), 2 do contrato do ABI (HLSL contra o `shader_common.h` do emissor
  corrigido) e 1 ponta a ponta (chamadas D3D sintéticas → front end → Vulkan → frontbuffer lido de volta).
  Os testes de GPU/contrato/ponta a ponta exigem DXC e, o contrato, `python tools/vkshaders/fetch_xenosrecomp.py`.
- Front end limpo sob ASan/UBSan e TSan (`SANITIZE=address,undefined CXX=g++ tests/test_pcvk.sh`).
- Os arquivos que dependem do SDK (`sr_vk_*.cpp`, `sr_native_system.cpp`) passaram em `-fsyntax-only`
  contra os headers reais do SDK; **não foi gerado NRO** (sem devkitA64, sem o jogo, sem console).

## Licença

Os dois projetos são do mesmo autor. O projeto PC não tem arquivo LICENSE; o código derivado do
[rexglue-native-kit](https://github.com/crazyriddler/rexglue-native-kit) mantém os avisos de origem
nos cabeçalhos. Ver `THIRD_PARTY_NOTICES.md`.
