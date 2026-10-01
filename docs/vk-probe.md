# Teste mínimo Vulkan/NVK e a falha do Sudachi

`tools/switch/vk-probe/` gera `vk-probe.nro`, um NRO sem código nem dados do jogo
que liga o mesmo Mesa NVK estático do jogo e percorre a inicialização gráfica
passo a passo. Cada passo é gravado e sincronizado no SD **antes** de executar
(`BEGIN passo`), então, se o emulador morrer, a última linha `BEGIN` indica a
operação. O log também sai por `svcOutputDebugString` (Sudachi: `Debug.Emulated`).

| Passo | O que exercita |
|---|---|
| `guest_memory` (opcional) | Janela de 4,5 GiB, as nove vistas de `xmemory.cpp` e o precommit de 512 MiB espelhado nas vistas, com as mesmas SVCs de `guest_memory_switch.cpp` (`MapProcessCodeMemory`, `SetProcessMemoryPermission`, `MapProcessMemory`), antes do Vulkan, como em `Runtime::Setup` |
| `instance`, `physical_device` | Instância com `VK_NN_vi_surface`, heaps, tipos de memória e filas |
| `device` | Canal NVDRV, ZCULL, primeiro envio de estado 3D/compute (macros MME incluídas) |
| `command_objects`, `empty_submit` | Pool, command buffer, fence; primeiro `vkQueueSubmit` + espera de 5 s |
| `copy`, `fill` | `vkCmdCopyBuffer` / `vkCmdFillBuffer` de 1 MiB conferidos na CPU |
| `clear_image` | Clear de imagem 256x256 e cópia para buffer, conferido na CPU |
| `draw`, `depth_draw` | Pipeline compilado pelo NAK, draw com teste de profundidade (plano ZCULL), pixel central conferido |
| `wsi` | Superfície `nwindow`, swapchain FIFO, N quadros apresentados |
| `teardown` | Destruição de todos os objetos |

O resultado final é `RESULT PASS` ou `RESULT FAIL (n failed steps)`.
Erros de conteúdo não interrompem a sequência; erros Vulkan interrompem.

## Configuração

Arquivo opcional `sdmc:/switch/superman-returns-nx/vk-probe.cfg`
(exemplos em `tools/switch/vk-probe/configs/`):

```text
env NOME=VALOR       variável do driver antes de vkCreateInstance
stop_after PASSO     encerra limpo depois do passo
skip PASSO           copy, fill, draw, depth ou wsi
frames N             quadros do passo wsi (padrão 120)
guest_memory MODO    reserve | eager (padrão: desligado)
```

## Compilação

```sh
tools/build-docker.sh mesa       # .tools/mesa-sdk (Mesa + os dois patches)
tools/build-docker.sh vk-probe   # out/probe/vk-probe.nro
```

Ou, com devkitPro e o Mesa SDK já presentes: `PROJECT=$PWD tools/switch/build-vk-probe.sh`.
Os shaders do probe estão embutidos (`probe.vert.inc`/`probe.frag.inc`), gerados
por `glslangValidator -V --target-env vulkan1.1 -x` a partir de `probe.vert`/`probe.frag`.

## Execução

Windows (Sudachi instalado):

```powershell
powershell -File tools/test-sudachi.ps1 -Nro out/probe/vk-probe.nro -Seconds 120 `
  -ProbeConfig tools/switch/vk-probe/configs/copy-engine.cfg
```

O script copia a configuração para o SD virtual, guarda `out/sudachi/vk-probe.log`
e falha se não houver `RESULT PASS`.

Linux sem desktop (`sudachi-cmd` compilado com `emulators/sudachi/build-linux.sh`):

```sh
SUDACHI_CMD=.../sudachi-cmd BACKEND=1 MACRO_JIT=0 \
  PROBE_CFG=tools/switch/vk-probe/configs/copy-engine.cfg \
  tools/switch/run-sudachi-headless.sh out/probe/vk-probe.nro 400
```

`BACKEND=2` usa o renderizador nulo do Sudachi: o processador de comandos da GPU
(GPFIFO, métodos, macros MME) roda, mas nada é desenhado nem copiado, então os
passos que conferem conteúdo falham por definição. `GDB=1` imprime o backtrace.

## Diagnóstico da falha `0xC0000005`

Reproduzida num build Linux do Sudachi (espelho do código-fonte, ver
`emulators/sudachi/README.md`) com o probe, sem o jogo: SIGSEGV no processo do
emulador logo após `END command_objects`, ou seja, ao processar o primeiro envio
do canal, de forma assíncrona. Backtrace: `Tegra::DmaPusher::CallMethod`,
`dma_pusher.cpp:199`, desreferência de `subchannels[n]` nulo.

Três problemas independentes foram encontrados, nesta ordem:

1. **Métodos em subcanal sem objeto (driver + emulador).** A camada Horizon do NVK
   (`nouveau_horizon_gm20b.c`) coloca no início de cada envio o "cache acquire"
   do deko3d: métodos da classe 3D (`0x4A2`, `0x369`, `0x50A`, `0x509`, invalidações
   de cache) no subcanal 0. No **primeiro** envio de um canal eles chegam antes do
   `SET_OBJECT(B197)` do fluxo de inicialização do NVK. Além disso o NVK nunca faz
   `SET_OBJECT` da classe de cópia no subcanal 4 (`NVK_COPY_ENGINE=1`). Sudachi
   não trata subcanal sem motor e encerra o processo. O caminho Vulkan ou OpenGL
   do host e a GPU assíncrona não mudam isso, como já observado no Windows.
2. **Macro JIT do Sudachi.** Com o canal corrigido, o primeiro draw nunca termina:
   a thread `GPU` fica a 100% dentro do código JIT de uma macro MME. Bisseção:
   desligar só a otimização `zero_reg_skip` resolve. Em `Compile_ALU`, uma operação
   cujo primeiro operando é `r0` não carrega nada no registrador de resultado, então
   `rD = r0 + r0` (como o construtor MME Fermi do NVK codifica "zero") e `rD = r0 + rB`
   produzem o valor da instrução anterior. `And` com operando zero também.
   Além disso `AddImmediate`/`Read` com imediato `2` não somam nada (`> 2` em vez
   de `>= 2`); a macro `0x03af` do NVK usa `r3 = r3 + 2`. O interpretador de macros
   do Sudachi executa as mesmas macros corretamente.
3. **Tamanho de storage buffer (heurística NVN).** `BufferCache::StorageBufferBinding`
   lê o tamanho do SSBO na palavra seguinte ao endereço no constant buffer 0, como
   o driver NVN faz. No shader de cópia do NVK essa palavra é o outro endereço:
   o Sudachi tentou criar um buffer de staging de ~4 GiB (`VK_ERROR_OUT_OF_DEVICE_MEMORY`
   e `abort` com lavapipe). Com o motor de cópia DMA isso não ocorre.

## Correções

| Camada | Mudança | Efeito |
|---|---|---|
| Mesa (este port) | `mesa/mesa-switch-superman.patch`: ao criar cada canal, um envio de uma entrada com `SET_OBJECT` das cinco classes GM20B nos subcanais do NVK (0: B197, 1: B1C0, 2: A140, 3: 902D, 4: B0B5), a ordem do deko3d, sem incremento de syncpoint. `NOUVEAU_HORIZON_EARLY_BIND=false` restaura o comportamento anterior. | Remove a falha do Sudachi 1.0.15; o fluxo de init do NVK repete os mesmos `SET_OBJECT` de 3D/compute. |
| Aplicativo | `superman_returns_app.h`: em lançamento direto (sem hbloader, como nos emuladores), padrão `NVK_COPY_ENGINE=1`. | Evita o item 3. Console via hbloader não muda. |
| Emulador | Configuração: **Emulation → Configure → Debug → Disable Macro JIT** (`disable_macro_jit=true`). | Contorna o item 2 no Sudachi instalado, sem alterá-lo. |
| Emulador (fork) | `emulators/sudachi/sudachi-nvk-compat.patch`: ignora método em subcanal sem motor (com log) e corrige o JIT (`r0` como operando e imediato 2). | Para quem compila o Sudachi; não é aplicado ao emulador instalado. |

## Evidência (2026-10-01, build Linux do Sudachi, Mesa e NRO compilados nesta sessão)

`stock` = espelho do Sudachi só com ajustes de compilação (`sudachi-linux-build.patch`),
equivalente em comportamento ao 1.0.15 nos trechos envolvidos. Backend do host:
Vulkan (lavapipe/llvmpipe, Xvfb). Configuração do probe: `NVK_COPY_ENGINE=1`, 30 quadros.

| Emulador | Driver | Macros | Resultado |
|---|---|---|---|
| stock | `EARLY_BIND=false` | JIT | SIGSEGV (139) após `END command_objects` |
| stock | com o patch | interpretador | **PASS**: 12 passos, pixels conferidos, 30/30 quadros apresentados |
| stock | com o patch | JIT (padrão) | trava no primeiro `draw` (timeout) |
| stock | com o patch + `guest_memory eager` | interpretador | **PASS**, 2.064 MiB espelhados, coerência entre vistas confirmada |
| com `sudachi-nvk-compat.patch` | com o patch | JIT | **PASS** |
| com `sudachi-nvk-compat.patch` | `EARLY_BIND=false` | JIT | sem falha do host; métodos ignorados e logados; cópia/clear/draw incorretos |
| stock | com o patch, cópia por shader (`meta-copy.cfg`) | — | `abort` por buffer de ~4 GiB (item 3) |

O mesmo build reproduz o probe de plataforma do Sudachi 1.0.15 do Windows
(39 bits, alias coerente, reservas e mapas GPU fixos e dinâmicos).

`maxwell_dma.cpp:75` registra asserts não fatais: o NVK usa transferência
`PIPELINED`, que o Sudachi não implementa de forma distinta.

## O que ainda não está validado

- O NRO do jogo **não** foi recompilado nem executado nesta sessão: o C++ gerado e
  os arquivos do jogo não estão no repositório. O próximo passo é o usuário rodar
  `vk-probe.nro` e o jogo no Sudachi 1.0.15 do Windows com "Disable Macro JIT".
- O binário Windows do Sudachi 1.0.15 (`sudachi-refresh-c7431bd`) não foi executado;
  o código usado é um espelho de 2024-09-19 com dependências da era yuzu.
- Console físico: o `SET_OBJECT` antecipado segue a ordem do deko3d e repete o que o
  NVK envia logo depois, mas não foi testado em hardware. Se houver regressão,
  `NOUVEAU_HORIZON_EARLY_BIND=false` volta ao comportamento anterior.
- A heurística NVN do item 3 também se aplica a SSBOs lidos de constant buffers
  diferentes de 0, com limite de 8 MiB. O renderizador Xenos usa storage buffers
  (memória compartilhada); isso pode truncar dados no Sudachi. Não foi medido.
- lavapipe/llvmpipe não representa uma GPU real; o desempenho e a imagem do jogo
  precisam ser medidos no Windows.
