# Checkpoint 3 — falha gráfica do Sudachi (2026-10-01)

Objetivo: rodar Superman Returns no Sudachi. Esta etapa isolou e corrigiu a
falha `0xC0000005` da inicialização gráfica, sem os arquivos do jogo.

## Concluído

- `vk-probe.nro` (`tools/switch/vk-probe/`): teste Vulkan/NVK mínimo, sem código
  nem dados do jogo, com o mesmo driver do NRO. Log por passo, sincronizado antes
  de cada operação; passo opcional que reproduz o layout de memória eager do jogo.
- Build Linux do Sudachi (espelho do código, `emulators/sudachi/`) reproduziu a
  falha: SIGSEGV em `DmaPusher::CallMethod` no primeiro envio do canal.
- Causa: o prólogo "cache acquire" da camada Horizon do Mesa envia métodos 3D ao
  subcanal 0 antes do `SET_OBJECT`; o NVK também nunca vincula a classe de cópia.
  Correção em `mesa/mesa-switch-superman.patch` (vínculo das cinco classes ao criar
  o canal, ordem do deko3d). Controle A/B no mesmo binário com
  `NOUVEAU_HORIZON_EARLY_BIND=false`.
- Problemas do emulador isolados: macro JIT calcula errado operações com `r0` e
  imediato 2 (draw do NVK em loop infinito); heurística NVN de tamanho de SSBO
  gera buffer de ~4 GiB para a cópia por shader. Contornos: "Disable Macro JIT"
  e `NVK_COPY_ENGINE=1` (padrão do app em lançamento direto). Patch opcional do
  emulador em `emulators/sudachi/sudachi-nvk-compat.patch`.
- Resultado no Sudachi equivalente ao instalado, com interpretador de macros:
  `RESULT PASS` em 12 passos (cópia, fill, clear, draw, profundidade, 30 quadros
  apresentados), também com 2 GiB de memória do guest espelhados antes do Vulkan.
- Infraestrutura: `tools/build-docker.sh` (Linux, sem dependência do Windows),
  proxy/CA opcional, `build-mesa.sh` ressincroniza a fonte a cada build, guarda em
  `rebuild.sh`, `fetch_thirdparty.py` no Linux, teste de registro sintético,
  testes de scripts. Detalhes em `docs/vk-probe.md`, `docs/building.md` e
  `docs/validation.md`.

## Falta validar (usuário, Windows)

1. Recompilar Mesa (`tools/build-docker.ps1 -DriverOnly` ou `.sh mesa`), o probe e o NRO.
2. No Sudachi 1.0.15, ativar Disable Macro JIT e rodar
   `tools/test-sudachi.ps1 -Nro out/probe/vk-probe.nro -ProbeConfig tools/switch/vk-probe/configs/copy-engine.cfg -Seconds 120`.
   Repetir com `eager-memory.cfg` e, como controle, `early-bind-off.cfg` (deve falhar).
3. Rodar o NRO do jogo e guardar `out/sudachi/` para a próxima etapa (memória
   eager, MMIO, primeiro PPC).

O NRO do jogo não foi executado nesta etapa; nenhuma imagem do jogo, título ou
gameplay foi validado. Console físico e Ryujinx não foram testados.
