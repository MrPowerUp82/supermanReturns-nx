# Renderer nativo Vulkan para gameplay — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Renderizar boot, vídeo, menu e gameplay de Superman Returns no Switch físico usando Vulkan nativo, com recursos limitados, sincronização correta e validação de imagem/desempenho.

**Architecture:** Capturar as chamadas D3D e seu estado utilizando os fatos confirmados do projeto PC. Associar cada operação capturada ao pacote PM4 correspondente e executá-la uma vez em Vulkan, preservando o sistema MMIO/ring/vblank do NX e vinculando conclusão guest às fences reais. Reutilizar o leitor `.srsp` e o provider/presenter Vulkan existentes.

**Tech Stack:** C++23, ReXGlue local, libnx/devkitA64, Vulkan/Mesa NVK, XenosRecomp, DXC, SPIR-V Tools, Docker e Python unittest.

**Spec:** `docs/superpowers/specs/2026-10-02-renderer-nativo-gameplay-design.md`, aprovada pelo usuário em 2026-10-02.

Estado: plano para revisão; nenhuma tarefa abaixo foi executada. A referência PC
é `257feabc050e03c287fdf6238bf55876e5b59d81`; NX antes da especificação é
`5c4822bc4e9ad50b31e43db1b9cb835e41cb4b93`, especificação inicial em `c29d421`.

## Global Constraints

- “Mantêm-se resolução original, Xenos selecionável por execução e a meta de 30 FPS nos clocks padrão, sem overclock.”
- “Não haverá fallback por draw para Xenos.”
- “Não liberar WAIT_REG_MEM por timeout, alterar SCRATCH para fabricar conclusão ou portar atalhos de espera do PC sem justificativa.”
- “A fila não pode depender de ponteiros guest mutáveis depois da captura.”
- “Uma entrega parcial nunca será relatada como conclusão do renderer.”
- “Containers, shaders traduzidos, C++ gerado, jogo e dumps ficam locais/ignorados.”
- “Não publicar assets, instalador, release ou alterar o projeto de PC nesta entrega.”
- Não copiar a redução de MSAA do PC como default. Opções HDR que mudam precisão devem permitir comparação.
- Preservar alterações alheias, configurações, builds e volumes Docker; não fazer limpeza ou reset geral.
- Antes de executar, ler a especificação inteira e aplicar a skill de worktrees; resolver o checkout e os mounts para esse diretório. Nunca copiar alterações do PC de volta para sua referência.

## Review Focus

1. DrawUP chama Begin/End internamente: emitir uma operação, inclusive em chamadas aninhadas; testes na tarefa 4.
2. Ring publica o pacote antes de o hook publicar o snapshot: bloquear sem avançar ou deadlock de locks; testar publicação atrasada/cancelamento e verificar as chamadas originais na tarefa 5.
3. Endereço de shader/buffer/segmento é reutilizado: identidade inclui geração/conteúdo, sem snapshots ou descritores antigos; tarefas 3, 5 e 9.
4. Resolve é lido pela CPU ou alias muda formato: readback/conversão devem terminar antes de concluir o efeito guest; tarefas 10 e 11.
5. Fechar a aplicação com fila cheia ou GPU pendente: cancelar produtores, juntar workers e conservar recursos ainda em uso; tarefas 6 e 12.

---

## Estrutura de arquivos e contratos comuns

Todos os novos componentes C++ ficam em `app/src/`, namespace `sr::native`.
O prefixo `sr_native_` distingue o caminho puro da rota híbrida do SDK.
Headers do núcleo host não incluem Vulkan, libnx ou headers de D3D12.

| Unidade | Arquivos | Responsabilidade |
|---|---|---|
| Perfil/memória | `sr_native_profile.h`, `sr_native_guest.h/.cpp` | Fatos Superman, leituras e cópias guest verificadas |
| Estado | `sr_native_capture.h/.cpp`, `sr_native_mirror.h/.cpp` | Snapshot imutável e estado PM4 do segmento D3D |
| Hooks | `sr_native_hooks.cpp`, `sr_native_bridge.h/.cpp` | ABI PPC e acesso ao serviço nativo; nenhum recurso Vulkan em hooks |
| Ordenação | `sr_native_commands.h/.cpp`, `sr_native_queue.h/.cpp` | Operações, associação ao PM4, backpressure e serial de conclusão |
| Shader/pipeline | `sr_native_shader_abi.h/.cpp`, `sr_native_pipeline.h/.cpp` | ABI refletida, especializações e criação de pipeline Vulkan |
| Buffers/texturas | `sr_native_geometry.h/.cpp`, `sr_native_texture.h/.cpp` | Decodificação CPU, snapshots e descrições de formatos |
| GPU | `sr_native_gpu.h/.cpp`, `sr_native_resources.h/.cpp` | Recursos/submissão Vulkan, uploads, fences e descartes |
| Superfícies | `sr_native_targets.h/.cpp`, `sr_native_resolve.h/.cpp` | RT/depth/alias e transferências GPU/CPU |
| Auxiliares | `shaders/native/blit.hlsl`, `alias.hlsl`, `depth_copy.hlsl` | Shaders próprios compilados offline para operações host |

Modificar `sr_native_ring.*`, `sr_native_system.*` e `sr_native_present.*` sem
remover os testes do marco 1. `ClearSequence` continua testável, mas deixa de ser
a saída do jogo quando o renderer completo estiver ativado. Não criar módulos
vazios antecipadamente: cada arquivo entra com sua tarefa e testes.

Tipos públicos são definidos uma vez em `sr_native_commands.h` (tarefa 2), depois
estendidos nas tarefas proprietárias; todas as assinaturas abaixo usam estes tipos:

- `Serial = uint64_t`: sequência crescente, zero significa nenhuma operação.
- `GuestAddress = uint32_t`; `GuestMemory` oferece cópias, nunca aritmética livre sobre `base`.
- `PacketSite { uint64_t allocation_epoch; uint32_t physical_address; }`: localização de um pacote em uma alocação de segmento. Ring generation e alloc epoch são validados pelo adaptador.
- `PacketStamp { PacketSite site; uint32_t header; std::vector<uint32_t> payload; }`: palavras host-order, comparação exata, sem aceitar só hash.
- `CommandKind`: draw, clear, resolve e swap; `NativeCommand` possui serial, stamps cobertos, estado e payloads possuídos via `std::variant`.
- `NativeResult`: complete, pending, cancelled, unsupported, invalid e failed. `complete` em execução indica GPU concluída; enqueue aceito usa serial e não este resultado.
- `CapturedState`: device, frame, arrays de fetch/ALU/bool/loop, render state, viewport/scissor, shaders e streams versionados. Arrays VS/PS possuem 256 float4 cada.
- `ShaderIdentity`: estágio + bytes originais ou identidade validada do registry; endereço de objeto isolado não é identidade.
- `DrawPayload`: primitiva, start/count, base vertex assinado, índice 16/32-bit/endian, dados inline e strides; `ClearPayload`: flags, retângulos, float4 color, depth e stencil; `ResolvePayload`: flags, source rect, dest texture/point/mip/slice, clear color/depth/stencil; `SwapPayload`: front-buffer identity, width/height. Todos possuem os dados referenciados, sem ponteiros guest pendentes.
- `ShaderInterface`: descriptors, locations, offsets, blocos e capabilities refletidos de um estágio; `ShaderAbi`: união validada das interfaces da biblioteca e sua impressão de versão. `ResourceLimits`: bytes máximos totais GPU, capturados CPU, comandos e uploads.

Valores iniciais de segurança, sujeitos à medição final: fila 512 operações e
8 MiB de payloads CPU; teto GPU de 192 MiB contabilizando images, buffers e
recursos retirados; uploads de no máximo 16 MiB por lote/slot. Upload maior é
dividido em lotes e uma operação isolada maior que o teto CPU é recusada com
diagnóstico, não fica esperando espaço impossível. Estes valores não são uma
estimativa do working set final. Falha de reserva para recursos indispensáveis
bloqueia a operação com razão, sem reduzir a qualidade silenciosamente.

## Comandos de verificação

Em PowerShell, a partir do checkout escolhido para execução:

```powershell
$taskRoot = (Get-Location).Path
$taskMount = "type=bind,source=$taskRoot,target=/project"
docker run --rm --mount $taskMount -w /project superman-returns-nx-mesa:build bash tests/test_sr_native.sh
python -m unittest discover -s tests -v
docker run --rm --mount $taskMount -w /project superman-returns-nx-mesa:build bash shaders/test_pack_identify.sh
docker run --rm --mount $taskMount -w /project superman-returns-nx-mesa:build bash shaders/test_registry.sh
git diff --check
```

Resultado host exigido: exit 0 de cada comando, todos os binários C++ executados
e zero falhas unittest. Estender `test_sr_native.sh` com os binários de cada tarefa,
usando fixtures sintéticos; não compilar unidades GPU nesse runner sem dispositivo.

O ambiente examinado tem imagem `superman-returns-nx-mesa:build` e volume
`superman-returns-nx-build`; não foi encontrado volume `superman-returns-nx-check`.
Confirmar o conteúdo do volume antes de usar rebuild. Para build completo real:

```powershell
tar -cf .tools/project-build-source.tar --exclude=app/out --exclude=sdk/out app sdk tools/switch/cmake
docker run --rm --mount $taskMount --mount 'type=volume,source=superman-returns-nx-build,target=/work' -e JOBS=4 superman-returns-nx-mesa:build bash /project/tools/switch/build-game.sh
```

Usar `tools/switch/rebuild.sh` só quando seu `/work/game-check/CMakeCache.txt` e
`/work/superman-source` existirem e tiverem sido configurados para a fonte certa.
O `compile-check.sh` usa uma biblioteca Vulkan stub e mascara a falha do build:
não é evidência de link correto. Cada build aceito exige NRO/ELF reais e saída 0
do build real; guardar o log integral e SHA-256 dos artefatos em `out/`.

## Tarefa 1: Perfil Superman e acesso guest seguro

**Files:** criar `app/src/sr_native_profile.h`, `sr_native_guest.h/.cpp`,
`tests/test_sr_native_profile.cpp`, `tests/test_sr_native_guest.cpp`;
modificar `tests/test_sr_native.sh`, `THIRD_PARTY_NOTICES.md`.

**Interfaces:** `GuestMemory::Copy(GuestAddress, uint32_t, std::vector<std::byte>&) const -> bool`,
`ReadU32(GuestAddress, uint32_t&) const -> bool`, `Write(GuestAddress, std::span<const std::byte>) -> bool`.
Callbacks do construtor verificam intervalo, commit e permissões; saída permanece
inalterada em falha. Produzir `DeviceLayout` e constantes de hooks em `profile`.

- [ ] Conferir assinatura SHA-256 do XEX (`c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b`), manifesto, funções geradas e evidência de cada papel em `PC/port/src/native_renderer/game_profile.h`; documentar proveniência por arquivo antes de importar.
- [ ] Escrever `ProfileMatchesSupermanLayout`: assert fetch `0x400`, VS `0x700`, PS `0x1700`, viewport `0x3058`, shader VS `0x3080`, PS `0x3084`, ring write `0x28`, limit `0x2C`. `CopyRejectsOverflowAndUncommittedPage`: leitura que cruza página inválida falha e não altera vetor anterior; Write recusa página read-only.
- [ ] Executar runner host; verificar falha pelos novos símbolos ausentes.
- [ ] Implementar as interfaces e o perfil; usar validação SDK no binding real e callbacks em testes. Não reutilizar `base + address` do PC sem validação. Revisar licenças; quando a origem não puder ser atribuída, escrever implementação própria pelo contrato SDK.
- [ ] Executar testes host e diff-check; commit apenas desses arquivos com `feat: add verified Superman native profile and guest access`.

## Tarefa 2: Espelho PM4 e snapshots de estado

**Files:** criar `sr_native_commands.h`, `sr_native_mirror.h/.cpp`,
`sr_native_capture.h/.cpp`, `tests/test_sr_native_capture.cpp`; modificar runner.

**Interfaces:** `StateMirror::Scan(const GuestMemory&, std::span<const std::byte>, PacketSite) -> NativeResult`,
`StateMirror::ResetSegment(uint64_t allocation_epoch) -> void`,
`StateCapture::Snapshot(const GuestMemory&, GuestAddress device, uint64_t frame, const StateMirror&, CapturedState&) -> NativeResult`.
Produz stamps e snapshots independentes da memória guest, com os tipos comuns.

- [ ] Escrever `InlineConstantsOverrideDeviceShadow`: SET_CONSTANT no segmento muda VS c3, apesar de shadow antigo; assert cópia host-order e versão diferente. `PartialPacketDoesNotMutateMirror`, `IndirectCycleIsInvalid`, `ResetSegmentDoesNotReplayPackets` e `SnapshotSurvivesGuestOverwrite` fixam limites, endianness e ownership.
- [ ] Rodar host; verificar falha dos testes novos.
- [ ] Implementar parser de espelho somente de observação, usando limites/decodificação compartilháveis do parser NX; referências indiretas passam por GuestMemory. Capturar os grupos `kRegisterShadow` confirmados do PC e viewport como uint32 x/y/width/height + float min/max depth.
- [ ] Rodar todos os testes nativos e UBSan quando disponível; unknown packet não pode tornar snapshot válido se seu efeito de estado for necessário.
- [ ] Commit de tarefa 2: `feat: capture immutable native D3D state and command stamps`.

## Tarefa 3: Corpus SPIR-V e identidade de shader

**Files:** modificar `shaders/build_library.sh`, `nfsmw_hlsl.cpp`,
`XenosRecomp/shader_recompiler.*` e `shader_common.h` quando a comparação exigir;
`app/src/sr_shader_registry.*`, `sr_shader_hooks.cpp`; criar
`shaders/import_pc_corpus.py`, `tests/test_import_pc_corpus.py`,
`tests/test_sr_native_shader_objects.cpp`; atualizar `docs/shaders.md`.

**Interfaces:** `ShaderRegistry::RememberD3DObject(uint32_t object, const ShaderIdentity&) -> void`,
`FindD3DObject(uint32_t object) const -> const Shader*`, `ForgetD3DObject(uint32_t object) -> void`.
CLI `python shaders/import_pc_corpus.py --reference <PC> --output <novo-out>`
produz containers e manifesto local com revisão/patches/hashes; nunca altera PC.

- [ ] Escrever `ObjectReuseReplacesIdentity`, `AmbiguousIdentityIsRejected`, `EmptyConstantTableAccepted`, `ImportRejectsDifferentStageOrTruncatedContainer` e `ImportDoesNotOverwriteOutput`. Fixture sintética contém header válido e CTAB zero, sem bytes do jogo.
- [ ] Rodar testes novos; obter falha antes da implementação.
- [ ] Implementar importação validada; conferir patches PC 0002/0004/0005/0006/0007/0008/0009 contra tradutor NX, aplicar só mudanças necessárias com regressões. Containers duplicados deduplicam por bytes+estágio; não presumir equivalência por hash. Guardar entradas originais no `.srsp` existente, não no `.srsl` DXIL.
- [ ] Gerar corpus em `out/native-shaders/<revisao>/` com o build_library adaptado e ferramentas DXC/spirv-val do ambiente de shaders; executar `test_translator.sh`, `test_pack_identify.sh`, `test_registry.sh`. Verificar cada SPIR-V com `spirv-val --target-env vulkan1.2 --scalar-block-layout`. Falha de tradução permanece explícita com identidade; nenhuma declaração de cobertura completa sem relatório do corpus.
- [ ] Commit apenas fontes/testes/docs: `feat: build native Switch shader corpus from PC containers`.

## Tarefa 4: Hooks D3D e bridge de captura

**Files:** criar `sr_native_hooks.cpp`, `sr_native_bridge.h/.cpp`,
`tests/test_sr_native_hook_capture.cpp`; modificar `sr_shader_hooks.cpp`,
`app/CMakeLists.txt`, runner e manifesto apenas se algum símbolo confirmado faltar.

**Interfaces:** `NativeCaptureBridge::BeginCall(CommandKind, GuestAddress device) -> CaptureCall`,
`EndCall(CaptureCall&, const GuestMemory&, NativeCommand&) -> NativeResult`;
`CaptureCall` guarda argumentos, cursor inicial, nesting e época do segmento.
`NativeCaptureBridge::SetEnabled(bool) -> void`; desabilitado é pass-through.

- [ ] Escrever `DrawUpWithNestedBeginEndCapturesOnce`, `IndependentInlineCallsDoNotSharePendingState`, `ArgumentsSurviveOriginalClobber`, `DisabledCaptureCallsOriginalOnce` e `ClearReadsFloatColorAndFloatDepth`. Fake original modifica r3/r4 e memória de shadow; asserts usam argumentos salvos e shadow após flush.
- [ ] Rodar host e verificar falha dos helpers novos.
- [ ] Implementar hook único por função confirmada e helpers host testáveis; DrawVertices `820FBBF8`, DrawIndexed `820FC000`, DrawUP `820FBBB0`, Begin `820FB6E8`, End `820FBBA0`, clear float4 `82101A58`, resolve `8210C5F8`, swap `82112050`. Conservar originais e ABI PPC, com estado inline por thread/escopo. Hooks shader integram registry existente, evitando símbolos duplicados.
- [ ] Cross-compilar/linkar NRO real; conferir hooks no ELF e chamadas originais conservadas. Inspecionar originais para confirmar que não esperam a fence do próprio pacote antes de EndCall; se houver tal caminho, publicar captura no ponto anterior à espera com evidência registrada, sem contornar a espera.
- [ ] Rodar host, registrar endereços e evidência em `docs/native-renderer.md`; commit `feat: intercept Superman D3D calls for native capture`.

## Tarefa 5: Associação exata de operações e pacotes PM4

**Files:** criar `sr_native_commands.cpp`, `tests/test_sr_native_order.cpp`;
modificar `sr_native_ring.*`, `sr_native_system.cpp`, mirror e bridge.

**Interfaces:** `CommandLedger::Publish(NativeCommand) -> Serial`,
`CommandLedger::Match(const PacketStamp&) -> MatchResult`,
`CommandLedger::Reset(uint64_t ring_generation) -> void`.
`MatchResult` traz NativeResult, serial e se o pacote é o último stamp da operação.
Services do ring recebem `native_packet(const PacketStamp&) -> PacketResult`;
sem callback, comportamento do marco 1 permanece. Expor localização correta em
indiretos, mantendo storage/retry sem efeitos duplicados.

- [ ] Escrever `DelayedCaptureKeepsDrawPending`, `OneOperationCoversNestedClearDrawsOnce`, `SameAddressNewEpochNeverMatchesOldCapture`, `MismatchNeverConsumesPacket`, `PredicatedDrawDoesNotExecute` e `IndirectStampUsesSourcePhysicalAddress`. Depois de publicar, retry consome exatamente uma vez; epoch diferente não casa mesmo com bytes iguais.
- [ ] Rodar host; conferir falha antes da associação nova.
- [ ] Implementar ledger com stamps de palavras exatas e época de alocação. PM4 de draw/clear/copy matching é um token da captura, não outro desenho; comandos sem captura ficam blocked. Associar wrap/reset e trocas RingMakeSpace/LargeSegment usando perfil confirmado. Não segurar mutex ledger durante originais, waits ou chamadas GPU.
- [ ] Acrescentar log local opt-in de stamps/call scopes e comparar uma sequência real de boot no Switch quando disponível. Exigir ausência de mismatch, replay, pacote sem operação e espera circular antes de ligar execução GPU. Esse trace pode permanecer blocked pelo marco 1; coletar o primeiro motivo sem fingir progresso.
- [ ] Rodar host/diff-check e commit `feat: correlate captured native operations with PM4 packets`.

## Tarefa 6: Fila limitada e conclusão real

**Files:** criar `sr_native_queue.h/.cpp`, `tests/test_sr_native_queue.cpp`;
modificar commands, `sr_native_ring.*`, `sr_native_system.*`, `sr_settings.*`.

**Interfaces:** `NativeQueue::Push(NativeCommand, const std::function<bool()>& cancelled) -> Serial`,
`Pop(NativeCommand&) -> NativeResult`, `Complete(Serial) -> void`,
`WaitThrough(Serial, const std::function<bool()>& cancelled) -> NativeResult`,
`Cancel() -> void`. Zero de Push é recusa/cancelamento, nunca sucesso.
Limites são contagem+bytes de snapshots. `Services::finish_native_work() -> PacketResult`
protege efeitos GPU-dependentes com o último serial correspondente já reconhecido.

- [ ] Escrever `QueueFullBlocksAndCancellationReleasesProducer`, `FenceBeforeFollowingDrawWaitsOnlyEarlierSerial`, `RetryDoesNotRepeatWriteback`, `CompletedSerialNeverPassesGap`, `PendingGpuDoesNotAdvanceEventWrite` e `ShutdownWakesConsumer` com executor falso controlado por latch.
- [ ] Rodar runner; verificar as falhas novas.
- [ ] Implementar queue/ledger ordering e gating de EVENT_WRITE_SHD, WAIT_FOR_IDLE, coerência e efeitos do SDK que dependem de GPU; MEM_WRITE puramente do CP conserva semântica documentada. Comparar cada evento com `sdk/src/graphics/command_processor.cpp`, sem tratar todo write como fence nem concluir serial de draw omitido com memexport. Acrescentar `OversizedCommandIsRejectedWithoutWaiting` para o limite individual de payload.
- [ ] Rodar testes multithread/UBSan; timeout do teste é falha, timeout GPU é diagnóstico/cancelamento. Definir cvars positivas de fila/bytes e logar limite; usar limite de teste injetado, padrão conservador documentado para boot, depois ajustar por medição na tarefa 14.
- [ ] Commit `feat: bound native command queue and gate guest GPU completion`.

## Tarefa 7: ABI SPIR-V e inputs de geometria

**Files:** criar `sr_native_shader_abi.h/.cpp`, `sr_native_geometry.h/.cpp`,
`tests/test_sr_native_shader_abi.cpp`, `tests/test_sr_native_geometry.cpp`;
modificar shader_common/emissor somente para manter ABI coerente.

**Interfaces:** `ReflectShader(std::span<const uint32_t>, ShaderInterface&) -> NativeResult`,
`BuildShaderAbi(std::span<const ShaderInterface>, ShaderAbi&) -> NativeResult`,
`BuildShaderConstants(const CapturedState&, const ShaderInterface&, ShaderConstants&) -> NativeResult`,
`DecodeGeometry(const GuestMemory&, const CapturedState&, const DrawPayload&, GeometryData&) -> NativeResult`.
ShaderConstants possui VS/PS 4096 bytes e shared alinhado ao maior offset refletido;
GeometryData possui bytes de streams/índices, atributos, topology e base vertex.

- [ ] Escrever `ReflectRejectsIncompatibleDescriptorSets`, `ConstantsPreserveBitsAndPadding`, `InputRemapFollowsPatchedFetch`, `QuadIndicesExpandWithCorrectWinding`, `IndexEndianAllFourModes`, `NegativeBaseVertexChecksRange` e `YInversionAppliedOnce`. Quad [0,1,2,3] vira [0,1,2,0,2,3], verificando culling na convenção adotada.
- [ ] Rodar host; conferir falha antes de implementar.
- [ ] Refletir tipos, bindings, locations, offsets e capabilities do SPIR-V validado. Manter interface atual: sets 0/1/2 para texturas 2D/3D/cube, set 3 samplers, set 4 bindings 0/1/2 para UBO VS/PS/shared, specialization id 0. Conferir shared por reflexão: o header atual declara 23 float4 mas contém índices de inv-size posteriores; corrigir emissor/UBO juntos se qualquer acesso superar a declaração. Não copiar `kSharedSize=512` sem provar o layout emitido.
- [ ] Implementar input de fetch e conversão/endian conforme referência, sem importar fallback Xenos; investigar se helpers de `sdk/src/graphics/vulkan/pack_shaders.*` podem ser compartilhados sem mover recursos Xenos. Estabelecer `-fvk-invert-y` no emissor e viewport positivo; documentar convenção de winding/depth e testar.
- [ ] Rodar host, regressões do tradutor e validar corpus regenerado; commit `feat: define native shader ABI and decode captured geometry`.

## Tarefa 8: Recursos e submissão Vulkan

**Files:** criar `sr_native_gpu.h/.cpp`, `sr_native_resources.h/.cpp`,
`tests/test_sr_native_resource_lifetime.cpp`; modificar present/system/CMake.

**Interfaces:** `NativeGpu::Initialize(NativePresentation&, const ShaderAbi&, const ResourceLimits&) -> NativeResult`,
`NativeGpu::Execute(const NativeCommand&) -> NativeResult`, `PollCompleted() -> Serial`,
`Shutdown(const std::function<bool()>& cancelled) -> NativeResult`.
`ResourceTracker::Retire(ResourceId, Serial) -> void`, `Collect(Serial completed) -> void`,
`TryReserve(ResourceClass, uint64_t bytes) -> bool`; byte counters incluem recursos retirados.
GPU Execute retorna pending após submissão; Complete na queue só após PollCompleted.

- [ ] Escrever `PendingSubmissionKeepsUploadAndDescriptorsAlive`, `PartialSetupDestroysOnlyCreatedObjects`, `DeviceLostDoesNotReportFrameComplete`, `BudgetIncludesRetiredImages` e `FenceFailureRetainsResources`, com funções GPU injetadas na parte de lifecycle.
- [ ] Rodar host; conferir falhas novas.
- [ ] Implementar alocação Vulkan, buffers host-visible, flush/invalidate conforme propriedades, pools/command buffers/fences por slot, registro de bytes e descartes por serial. Usar VulkanDevice/function tables existentes; habilitar features antes da criação do dispositivo somente quando usadas/refletidas. Serializar acesso a queue com o contrato do SDK/presenter. Não esperar em locks de captura ou queue.
- [ ] Build real NRO, testes host e probe Vulkan sem jogo para upload/barreiras/fence, integrado à ferramenta existente `tools/switch/vk-probe/`. Registrar capabilities/limites do console; ausência de feature exigida falha setup com diagnóstico.
- [ ] Commit `feat: execute native Vulkan submissions with bounded resource lifetime`.

## Tarefa 9: Texturas, samplers e invalidação dos buffers

**Files:** criar `sr_native_texture.h/.cpp`, `tests/test_sr_native_texture.cpp`,
`tests/test_sr_native_buffer_cache.cpp`; modificar geometry/resources/gpu.

**Interfaces:** `DecodeTexture(const GuestMemory&, std::span<const uint32_t, 6> fetch, DecodedTexture&) -> NativeResult`,
`BufferVersions::Refresh(const GuestMemory&, GuestAddress, uint32_t size, uint64_t frame) -> BufferVersion`,
`BufferVersions::Invalidate(GuestAddress, uint32_t) -> void`.
DecodedTexture usa GuestTextureFormat e metadados de mips/slices próprios; binding
Vulkan converte formatos e swizzles, sem incluir DXGI. BufferVersion contém geração/hash/dados possuídos.

- [ ] Escrever `TiledTextureRoundTripKnownCoordinates`, `MipAndCubeFacesStayWithinRange`, `UnsupportedFormatLeavesOutputUnchanged`, `SwizzleHandlesZeroAndOne`, `SmallCapeBufferChangesWithoutUnlock`, `ReusedAddressDoesNotReuseOldBuffer` e `CacheEvictsOnlyCompletedResources`.
- [ ] Rodar host e verificar falhas.
- [ ] Implementar tiling/endian/decode com matemática SDK e referência PC, validar base/mip/extents com overflow checks. Cobrir primeiramente os formatos registrados no corpus real; acrescentar teste por formato adicional requerido. Arrays 2D/3D/cube e fallback dummy descriptors devem ter tipo/layout válido. Sincronizar uploads e mudanças RT->sampled.
- [ ] Incorporar hash por frame para buffers de até 32 KB da correção da capa; buffers maiores exigem mecanismo de validade comprovado, não uma suposição de imutabilidade. Invalidar por Unlock, write tracking onde confiável e geração de alocação. Rodar regressões e build real.
- [ ] Commit `feat: decode native textures and refresh mutable guest buffers`.

## Tarefa 10: Pipelines, clear e superfícies HDR/profundidade

**Files:** criar `sr_native_pipeline.h/.cpp`, `sr_native_targets.h/.cpp`,
`shaders/build_native_helpers.sh`,
`tests/test_sr_native_pipeline_state.cpp`, `tests/test_sr_native_targets.cpp`;
modificar gpu, settings, resources e shaders auxiliares próprios.

**Interfaces:** `BuildPipelineKey(const CapturedState&, const ShaderInterface&, const GeometryData&, const TargetDescription&) -> PipelineKey`,
`TargetCache::Acquire(const TargetDescription&, Serial use) -> TargetHandle`,
`TargetCache::PlanAlias(const TargetDescription& old, const TargetDescription& next) -> AliasPlan`.
PipelineKey inclui shaders/ABI, atributos, topology, RT/depth formats, samples,
blend/masks, alpha/depth/stencil/cull e specialization bits; viewport/scissor são dinâmicos.

- [ ] Escrever `PipelineKeyChangesForDepthBlendStencilAndSamples`, `EquivalentKeyReusesPipeline`, `FloatClearPreservesHdrValues`, `ViewportUsesIntegerExtent`, `LdrBlackTo7e3DoesNotBecomeBlue`, `GuestFormats3And12ShareOnlyWhenOptionEnabled` e `SamplesFollowGuestByDefault`.
- [ ] Rodar host; verificar falhas novas.
- [ ] Implementar render passes/framebuffers/pipelines Vulkan, UBO descriptors conforme tarefa 7, clipping/alpha/depth/stencil, clears parciais e completos. Não descartar writes do PS por máscara equivocada. Iniciar com criação síncrona e cache em memória limitado; disco/prewarm só após medição justificar.
- [ ] Implementar alias/cópia de superfícies e correção HDR do PC com opções próprias reiniciáveis. Não reduzir MSAA guest por padrão; distinguir resolves multisample e single-sample. `shaders/build_native_helpers.sh <novo-output>` compila os shaders auxiliares próprios com DXC/spirv-val e emite header de uint32_t e manifesto das ferramentas; CMake consome o output ignorado e falha claramente se faltar. Fontes host próprias ficam no Git, sem blobs derivados do jogo.
- [ ] Build real + testes host, executar quadro sintético no probe com cor/HDR/depth/culling conhecidos quando console disponível; commit `feat: render native Vulkan draws with guest surface and pipeline state`.

## Tarefa 11: Resolves, readback guest e efeitos PM4

**Files:** criar `sr_native_resolve.h/.cpp`, `tests/test_sr_native_resolve.cpp`;
modificar targets, gpu, ring/system e shaders auxiliares alias/depth-copy.

**Interfaces:** `BuildResolvePlan(const CapturedState&, const ResolvePayload&, ResolvePlan&) -> NativeResult`,
`PackResolveReadback(const ReadbackImage&, const ResolvePlan&, std::vector<std::byte>&) -> NativeResult`;
`NativeGpu::Readback(const ResolvePlan&, ReadbackImage&) -> NativeResult`.
Plano declara ranges guest, mip/slice, retângulos, formato, clear e se conclusão
exige escrita CPU; escreve via GuestMemory somente após fence/invalidate.

- [ ] Escrever `ResolveSubrectAndMipWriteOnlyDestinationRange`, `DepthReadbackPreservesGuestEncoding`, `GuestWriteOccursOnlyAfterFence`, `RepeatedPendingPacketDoesNotResolveTwice`, `ResolveClearOccursAfterCopy` e `CoherDirtyWaitsForUploadsAndReadbacks`.
- [ ] Rodar host; verificar falhas.
- [ ] Implementar barreiras/layouts e resolves cor/depth/MSAA, readback staging, untile/retile/endian e clear associado. Superfícies resolvidas para sampling entram no cache pela identidade correta; retenção GPU não elimina a necessidade de memória guest coerente. Fixar semântica de CPU reads usando os contratos do SDK.
- [ ] Implementar queries realmente observadas com Vulkan query pools e publicação após conclusão. Memexport não suportado continua bloqueado; se presente nos cenários, implementar a escrita exigida e teste antes de aceite. Não usar zero/visibilidade fixa ou limpar COHER para escapar de bloqueador.
- [ ] Rodar host/build real e comparar bytes de resolves sintéticos e cenas instrumentadas com a referência local; commit `feat: complete native resolves and guest memory effects after GPU fences`.

## Tarefa 12: Apresentação da saída final, vídeo e encerramento

**Files:** modificar `sr_native_present.*`, system/bridge/gpu,
`superman_returns_app.h`, `sr_settings.*`, `sr_shader_hooks.cpp`, CMake;
criar `tests/test_sr_native_frame_lifecycle.cpp`; atualizar README/docs.

**Interfaces:** `NativePresentation::PresentImage(const PresentedImage&, uint32_t width, uint32_t height) -> NativeResult`.
PresentedImage contém view/layout/formato/dimensões, serial pronto e owner retido;
binding mantém view válida até o término de seu uso. `NativeCaptureBridge::Quiesce() -> bool`
desliga publicação e cancela produtores antes de GPU/presenter serem liberados.

- [ ] Escrever `SwapPresentsResolvedFrontBufferOnce`, `PresenterRetainsImageThroughItsSubmission`, `ResizeDoesNotReuseOldFramebuffer`, `NoLibraryFailsNativeSetup`, `CloseDuringFullQueueAndPendingFenceCancelsAllWorkers` e `XenosModeDoesNotEnableNativeCapture`.
- [ ] Rodar host e conferir falhas.
- [ ] Substituir clear preto por blit da saída final dentro de RefreshGuestOutput, usando imagens/versionamento/context do SDK e pintura na UI. Respeitar aspecto, gamma DC_LUT e faixa de cor; criar teste de rampa. Nunca capturar image_view do refresh para uso posterior ao contrato do presenter.
- [ ] Integrar OnPreSetup/OnPostInitLogging, flags native shader antes do setup Vulkan, biblioteca obrigatória no modo native e diagnóstico por modo. Atualizar milestone/build logs. Shutdown segue bridge/queue->workers->GPU->presenter; falha de drain retém recursos e informa incomplete.
- [ ] Rastrear vídeo EA: documentar qual decoder/upload/draw o guest usa; cobrir as operações verificadas com shaders/texturas nativas. Testar intro habilitada e skip-intro no console; não introduzir decoder NFSMW por hipótese. Rodar build/host e commit `feat: present native Superman frames and preserve shutdown and video paths`.

## Tarefa 13: Relatório, artefatos e aceite funcional no console

**Files:** modificar `tools/switch/native-report.py`, `tests/test_native_report.py`,
`docs/native-renderer.md`, README; criar `tools/switch/native-frames.py`,
`tests/test_native_frames.py`, `docs/native-gameplay-validation.md`.

**Interfaces:** CLI `native-frames.py <log> --scenario <nome> --output <json>`
lê swaps concluídos (serial/id/tempo monotônico), skips por motivo, bytes,
GPU/device failures e shutdown. Resultado é `failed`, `needs_console_review` ou
`performance_target_missed`; observações visuais manuais são registradas à parte.

- [ ] Escrever `BlackClearsNeverCountAsGameFrames`, `AppendedLogUsesLastRun`, `SkippedEssentialDrawFailsAcceptance`, `MissingFinalShutdownFails`, `PendingSubmissionIsNotCompletedSwap` e `ArtifactBuildMismatchFails` nas suites Python.
- [ ] Rodar unittest e observar falhas anteriores à ferramenta nova.
- [ ] Implementar contadores distintos captured/submitted/completed/presented/surface_paints; logar orçamento e uso de cada classe, serial pendente, shader/pipeline misses e tempos. Relatório mantém a semântica conservadora do marco 1; validar build/pack manifest e não publicar pass visual automaticamente.
- [ ] Gerar NRO e ELF do mesmo link, SHA-256 e MANIFEST em pasta nova `out/console/builds/<revisao>-native-gameplay/`, com configuração/pack/hash/toolchain associados. Auditar ELF para dependências Windows/D3D12 e hooks duplicados; repetir regressões Xenos/pack/host.
- [ ] Com usuário abrindo NRO e FTP disponível, preservar TOML e builds, subir nome novo e verificar download/hash. Rodadas: intro ligada, título/menu, início da cidade, movimento/capa/HUD, retorno e saída; comparar áudio/progresso com execução Xenos separada. Guardar captures/logs e corrigir primeiro bloqueador com teste antes de repetir.
- [ ] Completar ao menos 10 min contínuos sem crescimento ilimitado de memória, crash, áudio parado ou workers presos; marcar cada critério visual confrontado com captura Xbox 360 ou explicitamente pendente. Commit de ferramentas/docs apenas: `test: record native Switch gameplay acceptance and artifact checks`.

## Tarefa 14: Medição e otimização até a meta

**Files:** criar `sr_native_metrics.h/.cpp`, `tests/test_sr_native_metrics.cpp`;
modificar gpu/resources/settings e native-frames, testes Python e docs de validação.

**Interfaces:** `FrameMetrics::CompleteSwap(uint64_t id, Serial, uint64_t timestamp_ns) -> void`,
`FrameMetrics::Summarize() const -> FrameSummary`; summary possui mediana/p95,
fração >50 ms, tempo GPU por categoria e bytes por classe. GPU query timestamps
convertem usando timestampPeriod/validBits conferidos no dispositivo, sem fator fixo.

- [ ] Escrever `MetricsDoNotCountLoadingOrDuplicateSwap`, `TimestampWrapRespectsValidBits`, `IntervalsAreMilliseconds`, `TargetChecksExactThresholds` e `MemoryBudgetIncludesCapturedAndRetiredBytes`; fronteiras: mediana 33,4 ms, p95 35 ms, fração estritamente menor que 0,01 acima de 50 ms.
- [ ] Rodar host/unittest e conferir falhas.
- [ ] Implementar métricas de CPU/captura/upload/pipeline/wait/GPU/paint; reportar separadamente produção e exibição. Medir no console clocks reais, configuração e working sets antes de definir budgets finais. Fila/caches têm defaults documentados, rejeitam limites inválidos e mantêm backpressure cancelável.
- [ ] Medir 60 s parado e 60 s em movimento na mesma área, intro/loading excluídos; analisar os custos dominantes. Otimizar somente gargalo demonstrado, uma mudança por rodada: reúso de upload/descriptors, alias 7e3 ou pipeline prewarm quando medido. Cada alteração de imagem fica opcional, mantém sua comparação e repete aceite visual. Não alterar resolução ou clocks para atingir a meta.
- [ ] Registrar resultados, NRO/ELF/configuração/pack e limites finais; se faltar atingir a meta, manter a tarefa aberta e relatar o cenário/custo que falta. Commit de mudança comprovada com sua medição em vez de afirmar ganho estimado.

## Dependências, execução e fechamento

Ordem principal: 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13 → 14.
Tarefa 3 não precisa da fila, mas sua ABI define requisitos para tarefas 7/8/10;
evitar portar backend sem corpus refletido. Os traces reais da tarefa 5 e os
testes de console são gates de corretude, não uma autorização para omitir efeitos.

O usuário já aprovou a especificação; ainda precisa revisar este plano e escolher
o método de execução. Recomendação: execução inline com `executing-plans`, pela
dependência forte das interfaces e pelo trabalho contínuo de integração Vulkan.
Alternativa: `subagent-driven-development` com implementador/revisor por tarefa,
conservando a ordem e interfaces. Não dispatchar agentes antes dessa escolha.

No método inline, solicitar uma revisão independente ao concluir a implementação
e antes de integração, conforme as skills de execução/revisão; durante tarefas,
verificar cada contrato e corrigir findings antes de avançar. Não integrar/push
automaticamente por causa deste documento. Seguir o fluxo de finalização depois
de testes e aceite requeridos, preservando a autorização do usuário.

Checklist de fechamento:

- [ ] Perfil, captura, identidade e ABI correspondem ao Superman local.
- [ ] Stamps PM4 e operações associam sem replay e sem espera circular.
- [ ] Nenhuma fence/memória/query conclui antes do efeito GPU correspondente.
- [ ] Corpus válido, draws essenciais cobertos e imagens confrontadas com referência.
- [ ] Vídeos, menus, cidade, personagem, capa, HUD, áudio e entrada validados.
- [ ] NRO real linkado; ELF/pack/TOML/hashes do mesmo build preservados.
- [ ] 10 min estáveis, uso de memória limitado e saída com shutdown complete.
- [ ] Métricas 60 s parado/movimento atingem os limites aprovados nos clocks padrão.
- [ ] Revisão independente e regressão Xenos concluídas.
- [ ] Reportar renderer concluído somente quando os itens de implementação e validação forem satisfeitos.
