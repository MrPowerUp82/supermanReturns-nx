# Checkpoint 10 — renderer Vulkan do projeto PC integrado ao port — 2026-10-05

Branch `claude/exciting-hopper-wnd9iz`. Pedido: usar o renderer `native` com Vulkan de
[superman_returns_recomp](https://github.com/MrPowerUp82/superman_returns_recomp) (commit `f5ac13b`)
para terminar o port. **Nada foi executado em console ou emulador**: não há NRO desta etapa,
nem pack de shaders real, nem medição de desempenho. O código está integrado e testado no host.

## O que está implementado

- `app/src/pcvk/`: grafo de decodificação PM4, gravador Vulkan, contrato de shader
  `sr-vulkan-buffers-v1`, front end (captura nas threads do jogo → worker → `RenderPacket`),
  registro de shaders do jogo e **pack offline** de shaders (`SRVKPK01`, chave FNV-1a64 do container).
- `app/src/sr_vk_*`: hooks D3D (mesmos endereços do PC), acesso validado à memória do jogo,
  `Presentation` (usa o `VkDevice` e o presenter do SDK; compõe o frontbuffer na imagem de saída).
- `sr_native_ring` com modo `instant_gpu`: cumpre fences/write-back/contador de quadros; draws consumidos sem efeito.
- Ferramentas: `tools/vkshaders/` (XenosRecomp com 13 patches + DXC → SPIR-V → pack), `tools/pcvk/regen_helper_shaders.py`,
  `tools/project.py package --vulkan-shader-pack`.
- Opções: `sr_renderer="native"`, `sr_vk` (padrão true), `sr_vk_shader_pack`, `sr_vk_pipeline_cache`, `sr_vk_worker_lag`,
  `sr_vk_texture_per_frame_max_kb`, `sr_vk_large_texture_recheck_frames`. Legado: `-DSR_NATIVE_LEGACY_CAPTURE=ON`.
- Docs: `docs/pcvk-import.md`, `docs/native-renderer.md`, `docs/port-status.md`.

## Evidência (host)

- `tests/test_pcvk.sh` verde: 78 testes do guest/front end, 44 do Vulkan, 15 fixtures de GPU (lavapipe),
  2 de contrato do ABI, 1 ponta a ponta (chamadas D3D sintéticas → Vulkan → frontbuffer lido de volta).
- Front end limpo sob ASan/UBSan e TSan.
- Testes Python (`tests/test_vkshaders.py`, `tests/test_project.py`) e `tests/test_sr_native_ring.cpp` verdes.
- `-fsyntax-only` contra os headers reais do SDK e `aarch64-linux-gnu-g++ -D__SWITCH__` sobre todos os fontes pcvk.

## Não verificado

- Link/execução do NRO (devkitA64, Mesa NVK estático, ordem dos arquivos `.a`).
- Hooks `sr_vk_hooks.cpp` em execução real; layout de memória/páginas comprometidas na prática.
- Pack gerado a partir dos containers reais do jogo (cobertura; `misses()` indica o que falta).
- Desempenho, memória (orçamento ~3 GB; sem eviction de texturas), custo da validação de páginas e do hash de texturas por quadro.

## Próximos passos

1. Gerar o pack no PC a partir de `artifacts/shaders/raw`: `python tools/vkshaders/build_pack.py ...`
   e empacotar com `python tools/project.py package --vulkan-shader-pack <pack.srvk>`.
2. Compilar o NRO conforme `docs/building.md`; `sr_renderer="native"` em `config/superman_returns.toml`.
3. Rodar ~60 s no console e ler as linhas `[sr-vk] summary` (frames, composed, skipped_draws,
   skipped_shaders, capture_failures, pack misses) conforme `docs/native-renderer.md`.
4. Se `capture_failures` alto: revisar `SdkGuestAccess`. Se `skipped_shaders` alto: completar o pack.
   Se memória estourar: baixar `sr_vk_texture_per_frame_max_kb` e investigar eviction.
