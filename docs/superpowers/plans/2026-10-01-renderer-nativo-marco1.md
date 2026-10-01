# Renderer nativo: primeiro marco — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Executar o boot do Superman Returns no Switch full com sistema gráfico nativo, consumo PM4, sincronização correta e apresentação de tela limpa.

**Architecture:** Injetar um `rex::system::IGraphicsSystem` próprio em `OnPreSetup`, preservando a seleção Xenos por execução. Adaptar sistema/ring/apresentação do nfsmw-nx em unidades pequenas, sem hooks específicos do NFSMW. O primeiro marco contabiliza desenhos omitidos e bloqueia operações com efeitos guest ainda não implementados.

**Tech Stack:** C++23, ReXGlue, Vulkan/Mesa NVK, libnx, devkitA64, Docker e testes C++ de host.

**Spec:** `docs/superpowers/specs/2026-10-01-renderer-nativo-design.md` — aprovada pelo usuário em 2026-10-01. Ler integralmente antes de executar.

## Global Constraints

- "O desenvolvimento do jogo e a validação de imagem/desempenho acontecem no Switch."
- "Compilação, análise de código, tradução offline e testes de host continuam no computador".
- "A meta final é 30 FPS estáveis nos clocks padrão, sem overclock."
- "O Xenos permanece selecionável para diagnóstico e comparação."
- "A escolha vale por execução. Não alternar sistemas em runtime e não fazer fallback por draw do nativo para o Xenos."
- "Não alterar valores guest para simular conclusão."
- "Dados derivados do jogo não entram no repositório ou em artefatos publicados."
- "Cada rodada muda uma coisa por vez e conserva o NRO e configuração anteriores."
- Não implementar vídeo, pack draw, caches de textura ou otimizações de cena neste plano. A ampliação do pack continua como entrega separada.
- Preservar todas as alterações locais pré-existentes. Não executar reset, limpeza de checkout ou commits gerais.

## Review Focus

1. Pacote principal dividido por wrap ou publicação parcial: não consumir cabeçalho/payload antes de possuir o pacote inteiro; teste na tarefa 1.
2. Buffer indireto inválido/cíclico: não acessar memória fora do intervalo nem publicar conclusão; testes nas tarefas 1 e 2.
3. Espera nunca satisfeita: não avançar por timeout; cancelamento precisa terminar o worker sem falsificar sucesso; teste na tarefa 2.
4. Falha de setup parcial e repetição de shutdown: recursos e workers precisam ser liberados uma vez, em ordem; teste na tarefa 3.
5. Troca da geração do ring e falha de apresentação: não usar ponteiro antigo nem contabilizar apresentação falhada como quadro exibido; testes nas tarefas 3 e 4.

## Estado inicial e leitura obrigatória

HEAD ao iniciar o planejamento: `f02a9c9`. Há mudanças locais no app/SDK e arquivos novos do pack. Registrar `git status --short` antes de começar e preservar esse trabalho. O usuário não escolheu ainda o método de execução deste plano.

Referência local: `.tools/nfsmw-reference/`, revisão abreviada `df2de32`.
Registrar o hash completo com `git -C .tools/nfsmw-reference rev-parse HEAD`.
Ler `nfsmw_nativo_sistema.h/.cpp`, `docs/native-renderer.md` e `docs/measuring.md`.
Confirmar nomes de símbolos antes de copiar trechos; os pontos abaixo são âncoras,
não autorização para importar as dependências de 35 mil linhas.

| Referência | Reutilizar/adaptar | Excluir do marco |
|---|---|---|
| `Lector`, `Paquete`, `PaqueteTipo3`, `BuferIndirecto` | Decodificação PM4 e endianness | Saltos silenciosos após erro/limite |
| `SistemaGraficoNativo`, `SetupPresentation`, `SetupGuestGpu`, MMIO | Interface do SDK e infraestrutura de workers | `ActivarGanchos`, marcadores FlushState, biblioteca NFSP |
| `EscribirRegistro`, `LeerMemoria`, `EscribirMemoria`, `Interrupcion`, `BucleVblank` | Efeitos guest documentados | Escritas sintéticas sem justificativa |
| `Presentar`, `LimpiarSalida`, `DestruirVulkan` | Rota de clear C1 e recursos Vulkan | Destinos, vídeo, gamma, draw e contadores específicos de cena |
| `nfsmw_app.h::OnPreSetup` | Injeção em `config.graphics` | Hooks e entrada automática do NFSMW |

Comparar cada efeito com `sdk/src/graphics/command_processor.cpp` e
`sdk/src/graphics/graphics_system.cpp`. A referência abandona `WAIT_REG_MEM`
após 200 ms: **não copiar esse comportamento**. `COHER_STATUS_HOST` só pode
ser liberado depois de cumprir a coerência exigida, não por copiar o comentário
"não há memória compartilhada" da referência.

## Estrutura de arquivos e interfaces

Criar sob `app/src/`: `sr_native_ring.h/.cpp` (parser/estado/execução de pacotes),
`sr_native_system.h/.cpp` (SDK, memória, MMIO, workers),
`sr_native_present.h/.cpp` (Vulkan clear/apresentação). Testes sem SDK usam o
parser e callbacks falsos em `tests/test_sr_native_ring.cpp`; o adaptador tem
testes de lifecycle em `tests/test_sr_native_lifecycle.cpp`.

Modificar `app/src/sr_settings.h/.cpp`, `app/src/superman_returns_app.h`,
`app/CMakeLists.txt`, `THIRD_PARTY_NOTICES.md`. Criar
`tests/test_sr_native.sh`, `docs/native-renderer.md` e
`tools/switch/native-report.py`. Sem novos módulos vazios para os marcos futuros.

As interfaces abaixo são contratos a implementar, não símbolos já existentes:

```cpp
// sr_native_ring.h: puro C++, sem incluir tipos Vulkan ou runtime do SDK.
namespace sr::native {
enum class PacketResult { kConsumed, kIncomplete, kBlocked, kInvalid, kCancelled };
struct Cursor {
  std::span<const std::byte> bytes;
  uint32_t position = 0;
  uint32_t end = 0;
  uint32_t mask = 0;  // ring em palavras; zero para buffer linear
};
struct PacketView {
  uint32_t header = 0;
  std::vector<uint32_t> payload;  // palavras host-order
};
PacketResult PeekPacket(const Cursor&, PacketView&);
void CommitPacket(Cursor&, const PacketView&);
bool CompareWait(uint32_t info, uint32_t value, uint32_t reference, uint32_t mask);
bool ValidPhysicalRange(uint32_t address, uint64_t bytes);
struct Services {
  std::function<bool(uint32_t, uint32_t&)> read_register;
  std::function<bool(uint32_t, uint32_t)> write_register;
  std::function<bool(uint32_t, uint32_t&)> read_memory;
  std::function<bool(uint32_t, uint32_t)> write_memory;
  std::function<bool(uint32_t, uint32_t, std::vector<std::byte>&)> read_indirect;
  std::function<bool(uint32_t)> interrupt;
  std::function<bool(uint32_t, uint32_t, uint32_t)> present;
  std::function<bool()> cancelled;
  std::function<void()> pause_wait;
  std::function<void(uint32_t, uint32_t, uint32_t, uint32_t)> report_wait;
};
struct RingCounters {
  uint64_t packets = 0, indirects = 0, draws_omitted = 0;
  uint64_t swap_requests = 0, refresh_completed = 0;
  uint64_t interrupts = 0, blocked = 0, invalid = 0;
  std::array<uint64_t, 128> opcodes{};
};
class RingExecutor {
 public:
  explicit RingExecutor(Services services);
  PacketResult ProcessNext(Cursor&);
  const RingCounters& counters() const;
 private:
  Services services_;
  RingCounters counters_;
  std::vector<std::pair<uint32_t, uint32_t>> active_indirects_;
  struct IndirectFrame {
    uint32_t address = 0;
    std::vector<std::byte> storage;
    Cursor cursor;
  };
  std::vector<IndirectFrame> indirect_stack_;
  PacketResult Execute(const PacketView&, uint32_t depth);
};
}
```

Adicionar includes explícitos de `array`, `cstddef`, `cstdint`, `functional`,
`span`, `utility` e `vector`. Registros PM4 e opcodes vêm de
`rex/graphics/registers.h` e `rex/graphics/xenos.h` no `.cpp`; não inventar valores
numéricos quando há constantes do SDK. `Services` recebe endereços de memória
com os bits de endian originais; o adaptador interpreta-os como no SDK.

## Task 1: parser PM4 transacional e limites

**Files:** Create `app/src/sr_native_ring.h/.cpp`,
`tests/test_sr_native_ring.cpp`, `tests/test_sr_native.sh`.

**Interfaces:** Produz `Cursor`, `PacketView`, `PeekPacket`, `CommitPacket`,
`ValidPhysicalRange`; ainda não executa efeitos guest.

- [ ] Escrever testes que falham para cabeçalho tipo 2, tipo 0 com dois valores,
  tipo 1 com dois registradores, tipo 3 com payload, cabeçalho zero, wrap e pacote
  truncado. Usar palavras big-endian; fixture sem dados do jogo:

```cpp
static std::vector<std::byte> BE(std::initializer_list<uint32_t> words) {
  std::vector<std::byte> b;
  for (uint32_t w : words)
    for (int shift : {24,16,8,0}) b.push_back(std::byte((w >> shift) & 255));
  return b;
}
static void TestPartialAndWrap() {
  using namespace sr::native;
  auto b = BE({0x12345678u, 0, 0, 0xC0001000u}); // NOP no fim do ring
  Cursor c{b, 3, 0, 3}; PacketView packet;
  assert(PeekPacket(c, packet) == PacketResult::kIncomplete);
  assert(c.position == 3);
  c.end = 1;
  assert(PeekPacket(c, packet) == PacketResult::kConsumed);
  assert(c.position == 3 && packet.payload.at(0) == 0x12345678u);
  CommitPacket(c, packet);
  assert(c.position == 1);
  assert(!ValidPhysicalRange(0x1ffffffcu, 8));
  assert(ValidPhysicalRange(0x1ffffffcu, 4));
}
```

- [ ] Criar runner compilando apenas teste e `sr_native_ring.cpp`, com
  `-std=c++23 -Wall -Wextra -Werror -pthread`, includes do app/SDK. Executar
  `bash tests/test_sr_native.sh`; inicialmente deve falhar por símbolos ausentes.
- [ ] Adaptar `Lector` e contagem de payload de `Paquete`. Validar tamanho do
  span, máscara `2^n-1`, alinhamento, índices e soma de comprimento em 64 bits.
  Ring parcial retorna `kIncomplete` sem mudar posição. Linear truncado será
  `kInvalid` quando executado como indireto; uma tentativa incompleta principal
  é reavaliada depois de nova publicação do write pointer.
- [ ] Validar intervalo físico de 512 MiB depois de normalizar alias, sem deixar
  overflow ou wrap tornar intervalo inválido válido. `TranslatePhysical` apenas
  mascara o endereço; sozinho não é validação de acesso comprometido.
- [ ] Rodar os testes e repetir com ASan/UBSan no host. Commit somente arquivos
  desta tarefa: `feat: add transactional native PM4 parser`.

## Task 2: efeitos de pacotes, indiretos e esperas

**Files:** Modify `sr_native_ring.h/.cpp`, `tests/test_sr_native_ring.cpp`.

**Interfaces:** Consome parser; produz `Services`, `RingCounters`,
`RingExecutor::ProcessNext`, `CompareWait` e o estado persistente de indiretos.

- [ ] Criar fixtures com mapa de memória/registradores, vetor de interrupções e
  callbacks definidos em `Services`. Testar todos os comparadores 0–7,
  incluindo aplicação da máscara somente ao valor, conforme SDK:

```cpp
assert(sr::native::CompareWait(3, 0x1234, 0x34, 0xff));
assert(!sr::native::CompareWait(0, 1, 1, ~0u));
assert(sr::native::CompareWait(7, 0, 1, 0));
```

- [ ] Testar uma espera que permanece falsa por 1001 iterações e depois satisfaz,
  usando `pause_wait` para alterar o valor. Testar outra cancelada na iteração
  1001: resultado `kCancelled`, cursor não publica conclusão e memória inalterada.
  O número de iterações não equivale a tempo; nenhum limite de diagnóstico pode
  ser condição de sucesso. Rodar runner e confirmar falhas antes da implementação.
- [ ] Adaptar switch PM4 da referência comparando handlers do SDK para
  registradores, `REG_RMW`, `REG_TO_MEM`, `MEM_WRITE`, `COND_WRITE`, eventos,
  constantes, seleção/máscara de bins, shader loads e interrupções. Para cada
  handler importar sua leitura de campos e endian; testar os payloads mínimos
  e um payload menor que o mínimo. Não importar resultados sintéticos de queries.
- [ ] Classificar draw regular como omitido somente após validar seu formato;
  draw em modo copy/resolve com efeitos observáveis retorna `kBlocked` neste
  marco. Queries e operações GPU sem implementação também bloqueiam se o guest
  depender de resultado; não publicar números arbitrários para liberar o boot.
- [ ] Implementar `WAIT_REG_MEM` mantendo a condição, cancelamento e diagnóstico.
  Chamar `pause_wait` entre sondagens; o adaptador faz uma espera curta cancelável.
  `report_wait` é limitado por tempo no adaptador. Operações de coerência pendentes
  retornam bloqueio até existir implementação que realmente as cumpra.
- [ ] Em indiretos, validar endereço, bytes e profundidade máxima 4. Um par
  endereço/comprimento já ativo é ciclo inválido. Testar indireto truncado,
  ciclo A→B→A, tamanho que ultrapassa memória e profundidade 5. Manter um cursor
  persistente de indiretos parcialmente executados: operações com efeitos já
  executadas não podem ser repetidas quando um pacote posterior bloqueia.
- [ ] Para opcode desconhecido, retornar `kBlocked`, registrar opcode/endereço;
  nenhum `default` de sucesso. Testar predicado falso e swap predicado conforme
  SDK; garantir que callback de efeito não é chamado.
- [ ] Todos os testes passam; commit `feat: execute native PM4 guest effects`.

## Task 3: sistema gráfico, memória e lifecycle

**Files:** Create `app/src/sr_native_system.h/.cpp`,
`tests/test_sr_native_lifecycle.cpp`; modify runner e `app/CMakeLists.txt`.

**Interfaces:** Produz `std::unique_ptr<rex::system::IGraphicsSystem>
sr::native::CreateGraphicsSystem(bool configuration_valid = true)` e
`uint64_t sr::native::NativeProgress()`.
O progresso só aumenta após pacote processado, writeback válido ou interrupção
entregue; timer independente não é progresso do jogo.

- [ ] Extrair em `sr_native_system.h` um helper testável sem SDK de ciclo de vida
  `class WorkerStop` com `bool cancelled() const`, `void Stop()`,
  `void WaitFor(std::chrono::milliseconds)`. Usar atomic, mutex e condition variable.
  Testar worker parado durante espera, `Stop()` repetido e destruição após setup
  parcial; cada thread criada deve ser joined uma vez pelo proprietário.
- [ ] No header, forward-declare `rex::system::IGraphicsSystem` e manter os
  helpers independentes do SDK. Incluir a interface real somente no `.cpp` e
  em consumidores que precisam destruir o `unique_ptr`. Uma factory com
  `configuration_valid=false` fornece sistema cujo setup retorna
  `rex::X_STATUS_UNSUCCESSFUL` antes de criar recursos.
- [ ] Definir também `struct RingGeneration { uint32_t generation, read_word; }`
  e `void ResetOnGeneration(RingGeneration&, uint32_t)` para testar mudança de
  geração: se mudou, read_word vira zero; se igual, manter. Fixtures:

```cpp
sr::native::RingGeneration ring{1, 7};
sr::native::ResetOnGeneration(ring, 1); assert(ring.read_word == 7);
sr::native::ResetOnGeneration(ring, 2); assert(ring.read_word == 0);
sr::native::WorkerStop stop; stop.Stop(); stop.Stop();
assert(stop.cancelled());
```

- [ ] Rodar testes falhando, implementar helpers e adicioná-los ao runner.
- [ ] Adaptar a classe `SistemaGraficoNativo` para namespace `sr::native`,
  implementar todos os overrides de `IGraphicsSystem`. Portar MMIO e dimensões
  consultando `graphics_system.cpp`; validar `size_log2` antes de deslocamento
  e validar intervalo/alinhamento do ring e writeback antes de instalar.
- [ ] Fazer callbacks `Services` acessarem memória por `KernelState`/dispatcher,
  usando os heaps de `xmemory.h` para validar intervalo comprometido e proteção.
  Falha de leitura/escrita vira erro de diagnóstico, não dereference arbitrário.
  Preservar endian e efeitos SCRATCH conforme SDK.
- [ ] Portar worker do ring, `SetInterruptCallback`, interrupções e vblank.
  Atualizar write pointer com sincronização acquire/release; manter cursores
  principal/indiretos suspensos; não escrever read pointer além de operação
  bloqueada. Cancelar todas as esperas em shutdown e join antes de liberar recursos.
- [ ] Adicionar fontes explicitamente a `SR_SOURCES`. Compilar NRO com comando
  da tarefa 6; erros de imports devem ser resolvidos sem stubs de sucesso.
- [ ] Revisar semântica de interrupções antes de liberar callbacks após draws
  omitidos. O evento registra consumo, não execução visual completa; qualquer
  fence que exija efeito de memória GPU ainda ausente permanece bloqueado.
- [ ] Rodar host tests; commit `feat: add native graphics system and workers`.

## Task 4: apresentação Vulkan limpa

**Files:** Create `app/src/sr_native_present.h/.cpp`; modify system/CMake/runner
e `tests/test_sr_native_lifecycle.cpp`.

**Interfaces:** `class NativePresentation` com ctor/dtor e
`bool Initialize(rex::ui::WindowedAppContext*)`,
`bool PresentClear(uint32_t width, uint32_t height)`, `void Shutdown()`,
`rex::ui::GraphicsProvider* provider() const`,
`rex::ui::Presenter* presenter() const`. Definir a classe no namespace
`sr::native`; esconder recursos Vulkan no `.cpp` com `struct State` e
`std::unique_ptr<State>` para manter teste de lifecycle simples.

- [ ] Separar contagem de swap recebido, refresh/submissão concluída e pintura
  real na superfície. Não chamar `refresh_completed` de FPS apresentado.
  Reaproveitar métricas do presenter para confirmar exibição, registrando as
  três contagens com seus significados em `docs/native-renderer.md`.
- [ ] Adicionar helper puro `void RecordRefresh(bool ok, RingCounters&)` em
  `sr_native_ring.h/.cpp`; teste: `ok=false` mantém `refresh_completed=0`,
  `ok=true` aumenta em 1. Rodar falhando antes de implementar. Swap recebido
  aumenta `swap_requests` no executor, independentemente do resultado de refresh.
- [ ] Extrair apenas a rota C1 de `Presentar`/`LimpiarSalida` da referência: criação
  de provider/presenter em UI thread, command pool/buffer, fence, render pass,
  framebuffer por versão de imagem, clear e submissão protegida pelo lock da
  fila. Usar `rex/ui/vulkan/provider.h`, `presenter.h`, `device.h` existentes.
- [ ] Clear preto opaco constante. Usar render pass com `LOAD_OP_CLEAR`,
  `VulkanPresenter::kGuestOutputFormat`, barreiras de aquisição/liberação para
  `kGuestOutputInternalLayout` e masks expostas pelo presenter. Imagem de output
  não promete `TRANSFER_DST`, portanto não substituir por `vkCmdClearColorImage`
  sem conferir suporte/usage.
- [ ] Verificar cada `VkResult`: erro cancela trabalho e é registrado. Timeout
  de fence produz diagnóstico e permite cancelamento; não resetar ou destruir
  recurso ainda em uso. Device lost encerra o sistema e usa tratamento do SDK.
- [ ] Associar apresentação a `PM4_XE_SWAP`, não a um timer independente; separar
  vblank de swaps. Sem shader pack, vídeo, destinos ou gamma neste caminho.
- [ ] Testar shutdown com provider criado mas presenter ausente, pool criado
  mas framebuffer ausente, e tentativa de present falhada via callbacks falsos
  de aquisição/submissão. Compilar e rodar host tests; commit
  `feat: present native boot frames through Vulkan`.

## Task 5: seleção do renderer e diagnósticos

**Files:** Modify settings, app header, `THIRD_PARTY_NOTICES.md`; create
`docs/native-renderer.md`.

**Interfaces:** Cvar string `sr_renderer`, requires restart; `OnPreSetup`
injeta `CreateGraphicsSystem()`. `pack_shaders` segue controlando somente a rota
híbrida Xenos existente neste marco.

- [ ] Acrescentar declaração em `sr_settings.h` e definição em `.cpp`:

```cpp
REXCVAR_DEFINE_STRING(sr_renderer, "xenos", "Superman Returns",
                     "Graphics backend: xenos or native")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
```

- [ ] Em `OnPreSetup`, selecionar o sistema com este fluxo; o erro de setup
  é propagado por `sdk/src/ui/rex_app.cpp::SetupPresentation`, que já verifica
  `XFAILED(status)`. Usar `<string>` e `sr_native_system.h` no app header:

```cpp
void OnPreSetup(rex::RuntimeConfig& config) override {
  const std::string mode = rex::cvar::GetFlagByName("sr_renderer");
  if (mode == "xenos") return;
  const bool valid = mode == "native";
  if (!valid) REXLOG_ERROR("Invalid sr_renderer: {}", mode);
  config.graphics = sr::native::CreateGraphicsSystem(valid);
}
```
- [ ] Registrar modo, marco, hash/build e resumo a cada 10 s; quando bloqueado,
  registrar opcode, cursor, condição, último progresso e tempo. Limitar repetição
  por chave sem perder o primeiro evento e o resumo final.
- [ ] Documentar que tela preta é o resultado visual esperado do marco 1,
  testes inválidos quando há bloqueador e ausência de garantia de 30 FPS.
  Preservar avisos dos arquivos importados; adicionar referência/commit/licença
  a `THIRD_PARTY_NOTICES.md`.
- [ ] Revisar ausência de endereços/hacks NFSMW importados com busca textual:
  `rg -n '8259BC90|8259C038|825A40C0|ActivarGanchos|nfsmw_shaders' app/src/sr_native*`.
  Esperado: nenhum uso ativo. Compilar; commit
  `feat: select native renderer and report boot progress`.

## Task 6: build reproduzível e coleta do teste físico

**Files:** Create `tools/switch/native-report.py`; test
`tests/test_native_report.py`; document procedure in `docs/native-renderer.md`.

- [ ] Criar parser `parse_report(text: str) -> dict` com status
  `"blocked" | "failed" | "needs_console_review"`, nunca `"pass"` baseado só
  em FPS ou apresentação. Contrato de log: resumo
  `[sr-native] packets=... swaps=... refreshes=... draws_omitted=... blocked=... invalid=...`
  e eventos `[sr-native] failure=...`/`shutdown=complete`.
- [ ] Escrever testes unittest com strings sintéticas: bloquear apesar de 120
  refreshes; falha Vulkan; relatório sem progresso; shutdown ausente; relatório
  limpo exige `needs_console_review` pois a observação guest é manual. Fixtures:

```python
def test_refresh_does_not_hide_blocked(self):
    text = "[sr-native] packets=10 swaps=120 refreshes=120 draws_omitted=2 blocked=1 invalid=0\n"
    self.assertEqual(parse_report(text)["status"], "blocked")
```

- [ ] Executar `python -m unittest discover -s tests -p test_native_report.py -v`
  falhando, implementar parser e CLI `python tools/switch/native-report.py LOG`.
  CLI não modifica SD/config e não publica artefatos.
- [ ] Rodar testes C++ dentro do container Linux, evitando bash do Windows/WSL:

```powershell
docker run --rm --mount "type=bind,source=C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx,target=/project" -w /project superman-returns-nx-mesa:build bash tests/test_sr_native.sh
docker run --rm --mount "type=bind,source=C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx,target=/project" --mount type=volume,source=superman-returns-nx-check,target=/work -e JOBS=4 devkitpro/devkita64:latest bash /project/tools/switch/rebuild.sh
```

- [ ] `rebuild.sh` é incremental. Se faltar `/work/game-check/CMakeCache.txt`,
  usar o bootstrap existente `tools/switch/compile-check.sh` após ler seus
  argumentos; não apagar o volume para solucionar ausência de setup.
- [ ] Copiar o NRO compilado para nome novo `superman_returns-native-boot-test.nro`,
  guardar ELF correspondente em `out/console/symbols/`, registrar tamanho/SHA-256.
  Não usar ELF de outro NRO para simbolizar falha.
- [ ] Por FTP autorizado `192.168.100.37:5000`, fazer `cwd` no diretório antes
  de LIST. Baixar backup do TOML e verificar a versão atual antes de alterar
  somente `sr_renderer`. Upload do NRO novo; comparar tamanho/hash por download.
  Não sobrescrever os NROs anteriores ou qualquer pacote do jogo.
- [ ] Preparar duas rodadas equivalentes: Xenos baseline e native, cada uma
  com boot/60 s/saída manual em full. O usuário abre o NRO. Coletar logs/config
  e relatório de áudio/progresso/saída em pasta local nova com data/build/modo.
  Se bloqueado, solicitar saída e baixar o log; não insistir em mudanças de
  sincronização para aparentar progresso. Restaurar somente a chave alterada
  quando isso preservar mudanças posteriores do usuário.
- [ ] Commit somente script/teste/docs: `test: collect native boot diagnostics`.

## Task 7: aceite do marco e passagem

**Files:** Update `docs/native-renderer.md`; create `checkpoint6.md`.

- [ ] Rodar testes de host, compile e `git diff --check`. Executar teste sintético
  existente `bash shaders/test_pack_identify.sh` no Linux para confirmar que a
  identificação híbrida não regrediu; não reconstruir o pack nesta tarefa.
- [ ] Conferir logs do console e observação manual contra todos os critérios da
  spec: consumo PM4, progresso guest real, apresentação, sem bloqueadores,
  encerramento e outra execução Xenos preservada. Se um critério falhar, o marco
  fica incompleto; corrigir a causa na tarefa responsável e repetir o teste afetado.
- [ ] No checkpoint, registrar build/config/ELF, testes, opcodes observados,
  draws omitidos, esperas e evidência de progresso; separar fato de hipótese.
  Sem alegar vídeo correto, gameplay ou FPS final neste marco.
- [ ] Registrar os limites que orientam os próximos desenhos: vídeo EA,
  contêineres capturados para o pack e efeitos GPU bloqueadores observados.
  Commit somente documentação: `docs: record native renderer boot milestone`.

## Revisão do plano e método de execução

Este plano termina na primeira entrega da especificação. As outras entregas
não são tarefas escondidas dentro dela. Revisar interfaces, testes de falha,
efeitos guest e lifecycle antes de cada build físico.

Recomendação: execução pelo agente principal nesta sessão, com revisão independente
ao final, usando `superpowers:executing-plans`. As tarefas compartilham contratos
de ring, memória e apresentação, e o teste físico depende da mesma sequência de
logs; manter um implementador reduz custo de passagem de contexto. A alternativa
é execução por subagentes com revisão por tarefa, usando
`superpowers:subagent-driven-development`.

Aguardar revisão deste documento e escolha do usuário antes de implementar.
