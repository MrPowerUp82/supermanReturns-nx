# Instruções para Claude Cloud — Superman Returns NX

Este documento é o briefing da continuação. Trabalhe no repositório
https://github.com/MrPowerUp82/supermanReturns-nx e entregue as alterações em PR.
Objetivo principal: executar Superman Returns no Sudachi. Ryujinx é comparação;
Switch físico também é alvo, mas ainda não foi testado. Preserve o trabalho atual.

## 1. Receber a base correta antes de começar

Durante a preparação deste documento, o trabalho foi commitado e publicado:
o HEAD remoto verificado é `aa709b08882bac24a1b652234bb33074b137d55b`, na `main`.
Comece por esse commit ou um descendente que preserve suas alterações.
Ele já contém carregador/registro/hooks, correções do tradutor e dos emuladores,
testes e checkpoint2. Este briefing é um arquivo local adicional que o usuário
pode anexar ou copiar como prompt. Não é necessário aplicar novamente um patch
das alterações já presentes nesse commit.

Leia primeiro `checkpoint2.md`, `docs/shaders.md`, `docs/validation.md`,
`docs/building.md`, `docs/provenance.json`, `docs/port-status.md` e AGENTS.md,
se presente. O checkpoint1 contém histórico; checkpoint2 descreve a etapa atual.

Arquivos ignorados não chegam pelo clone nem pelo patch:

- `app/generated/default/`: C++ PPC recompilado e stamp do manifesto.
- `assets/` e a cópia própria de `game/default.xex` + 12 `DATA/*.AST`.
- `../superman_returns_recomp/logs/image.bin`: imagem XEX descompactada usada na
  análise e na extração de nove contêineres adicionais; receita do dump ainda
  não foi estabelecida neste projeto.
- `out/shaders-cube-review/`: contêineres, HLSL, SPIR-V, biblioteca e logs.
- `.tools/mesa-sdk/`, `.tools/mesa-switch/`, `.tools/dxc/`, exports das dependências
  e volumes/cache Docker.
- `out/sudachi/` e `out/ryujinx/`: evidências de execução local.
- O projeto PC `../superman_returns_recomp`, usado como referência adicional.

Para confirmar endereços/hook no executável, é necessário receber os fontes PPC
gerados ou a imagem apropriada; para regenerar shaders, os arquivos próprios do
jogo; para testes visuais, um ambiente de execução. Solicite só os insumos da
etapa que estiver executando. Não invente endereços sem esses insumos. Não inclua
dados originais do jogo, shader packs privados ou C++ gerado no PR público.
Se a nuvem não puder executar emuladores/console ou usar Docker, entregue os
testes executáveis e registre essa limitação; não declare runtime validado.

## 2. Estado comprovado

- Codegen ReXGlue v0.10.0: 293 arquivos, 145 C++, sem `REX_FATAL`.
- SDK, jogo e Mesa compilados para ARM64/libnx. NRO local: 59.865.285 bytes.
- Mesa switch fixado em `1a8c1a66d6fd8d65f10107c4627ffc3606ba5631`.
- Correção do tradutor CUBE: operandos/swizzle, absoluto/negação, constantes
  relativas e máscara parcial. Oito testes sintéticos passaram com DXC/spirv-val.
- Biblioteca: 529 contêineres traduzidos/compilados/validados, 167 entradas únicas.
  Arquivo `out/shaders-cube-review/superman_returns_shaders.srsp`, 9.081.800 bytes,
  SHA-256 `26d2c05bb357507be0264da585f643e517695ffac66b72383d817db5036ce181`.
- O NRO carrega a biblioteca ao lado do executável antes da inicialização gráfica.
  Sudachi e Ryujinx registraram 167 entradas carregadas.
- Registro de recursos: testes C++ passaram nas 167 entradas, padding externo,
  truncamento, assinatura desconhecida, reutilização de endereço e reload.
- Nove testes Python passaram. O empacotador aceita `--shader-library`.
- Pacote local completo: `dist/superman-shader-registry/`.

**Limite central:** os draws continuam usando Xenos. Carregar a biblioteca e
identificar recursos não significa executar esses SPIR-V na GPU. Nenhuma imagem,
tela de título, gameplay ou desempenho foi validado no Switch/Sudachi/Ryujinx.

## 3. Ordem de trabalho e entregas

### A. Reprodução e testes sem dados do jogo

1. Confirmar que as alterações recebidas estão presentes e que Python passa.
2. Reproduzir o teste CUBE e registrar as versões das ferramentas.
3. Acrescentar testes sintéticos relevantes ao registro/carregador, quando
   necessário para testar novas mudanças sem depender de um pack privado.
4. Tornar o build reproduzível num ambiente limpo e documentar pré-requisitos.
   `rebuild.sh` é incremental: depende de `/work/superman-source` e
   `/work/game-check` já preparados. `compile-check.sh` usa um libvulkan.a vazio
   e tolera a falha de link; não é evidência de NRO compilado com sucesso.

Aceite: testes realmente executados identificados, erros propagados corretamente,
instruções para build limpo e nenhuma dependência escondida do Windows local.

### B. Desbloquear inicialização gráfica no Sudachi

Prioridade principal. O probe sem jogo passa: espaço CPU de 39 bits, alias CPU
coerente, NVDRV/channel, reservas e mapas GPU fixos/dinâmicos. O NRO do jogo
derruba Sudachi 1.0.15 com `0xC0000005`, após vincular ZCULL, antes da memória PPC.
Evidência atual: `out/sudachi/superman_returns_016.log`; carregamento da biblioteca
na linha 2. OpenGL no host, GPU assíncrona desativada e
`NVK_SWITCH_MAPPED_COMPLETION=false` não resolveram nas tentativas locais.

1. Reduzir para um teste mínimo Vulkan/NVK que crie device, recursos, envie
   comandos e apresente/encerre, sem arquivos do jogo.
2. Determinar a primeira operação que desencadeia a falha com logs e, quando
   disponível, debugger do host. A hipótese de erro do cache GPU do emulador
   não está provada; não tratá-la como diagnóstico concluído.
3. Distinguir erro do driver, uso do aplicativo e implementação do emulador.
4. Corrigir na camada comprovadamente responsável. Workaround deve ser limitado,
   documentado e manter o caminho do console físico.

Aceite: teste mínimo funciona, device/swapchain e primeiro submit/present passam,
e o NRO avança além da inicialização gráfica. Processo aberto ao timeout não basta.
Se a correção exigir um fork do emulador, explicitar o repositório/patch separado
e a dependência; não fingir que mudar o port corrige o emulador instalado.

### C. Ryujinx: reserva fixa de endereço GPU

Problema independente, isolado no probe. Canary 1.3.351 inicia NRO direto com
36 bits CPU. Reserva GPU fixa `0x400010000` devolve `0x1000` com sucesso; reserva
`0x1fffff0000` devolve `0x400020000`. Map no endereço pedido falha com `0x195c`.
Reserva dinâmica + map passa. No jogo ocorre `ErrorOutOfDeviceMemory` ao criar
device. Evidência: `out/ryujinx/superman_returns_003.log`.

1. Confirmar contrato/retorno de NVDRV AllocSpace e manter distinção entre VA
   CPU e GPU. Os 36 bits CPU não explicam por si só o erro observado na GPU.
2. Avaliar reserva dinâmica com tradução consistente dos endereços lógicos no
   driver, ou corrigir a implementação do emulador. Alterar só o endereço do map
   não basta: allocator, limites, free, bindings e teardown devem concordar.
3. Testar reserva, mapa, unmap, liberação, reutilização e limites em ambos os
   emuladores, sem afetar console. Depois repetir o teste Vulkan mínimo.

### D. Memória PPC, MMIO e execução do jogo

`switch_eager_memory` mapeia chunks antecipadamente em lançamento sem hbloader;
alias via handle real obtido por IPC passou nos dois emuladores. A rota normal
depende de data aborts para mapear páginas, MMIO e write watch. Sudachi não
entregou os faults esperados. O caminho eager completo ainda não foi validado.

Revisar `sdk/src/core/guest_memory_switch.cpp/.h` e
`sdk/src/system/xmemory.cpp`: aliases físicos/virtuais do 360, offset de tradução,
commit/protect/decommit, coerência, comportamento por heap e limites. Resolver
MMIO/write watch com semântica explícita quando faults não estiverem disponíveis.
Não usar página RW global como substituto de emulação MMIO/proteção.

Aceite: testes direcionados de memória e primeiro PPC executado, carregamento AST,
threads e avanço ao título, com logs suficientes para localizar bloqueios.
Preservar padding TLS de `app/src/main.cpp` e proteção do profiler em
`sdk/src/ui/switch_perf.cpp` até demonstrar que podem ser removidos com segurança.

### E. Ligar os shaders pré-compilados à renderização

O hook atual em `app/src/sr_shader_hooks.cpp` observa `sub_82383AA8`. Evidência
estática: r4 é descritor tipo 15, +48 ponteiro, +52 tamanho; a função verifica
`0x102A1100` e o bit de estágio, retorna recurso do engine em r3. A identificação
ocorre antes da função original e o retorno é registrado depois. A função
original sempre executa. **Recurso do engine não é objeto D3D/pipeline Vulkan.**

1. Confirmar no executável do Superman os construtores, bind e destruição de
   shaders, vertex declarations e o vínculo entre recurso do engine e D3D.
   Registrar evidência por função: endereço, ABI/argumentos, chamadas e retorno.
   Os candidatos `826A8050/827A6130` do perfil PC permanecem não confirmados.
   Os hooks de NFSMW `8259BC90/8259C038` pertencem a outro jogo.
2. Capturar o contêiner completo antes de alterações dinâmicas de microcódigo;
   identificar por hash com comparação integral. Não usar só código físico.
3. Implementar renderer Vulkan compatível com a interface dos SPIR-V offline:
   descriptors, 256 constantes float4 VS/PS, boolean/int/loops, vertex input,
   interpoladores, estágios, specializations, samplers e texturas. Levantar a
   interface real dos shaders e da referência antes de fixar layouts.
4. Implementar pipeline/cache e estados de blend/depth/stencil/raster/viewport,
   topologia, formatos de targets, index/vertex buffers, instancing e draws.
5. Implementar formatos/endian/tiling de texturas, cubemap/mips, upload/sincronização,
   render targets, resolve, barreiras e vida útil/destruição de recursos.
6. Definir fallback explícito para shaders desconhecidos/sem CTAB e operações
   ainda não suportadas. Há 18 candidatos sem CTAB no dump, rejeitados pelo
   tradutor; serem clear/resolve internos é apenas uma hipótese.
7. Usar opção/configuração para comparar Xenos e nativo e evitar ativar uma rota
   incompleta por padrão. Não substituir diretamente um módulo Xenos por SPIR-V
   offline: o ABI de constantes/texturas é diferente.

Referências: `app/src/sr_shader_library.*`, `sr_shader_registry.*`,
`sdk/src/graphics/vulkan/`, `shaders/XenosRecomp/`; o projeto PC tem
`port/src/native_renderer/`. NFSMW é arquitetura de referência, não uma lista
de endereços/efeitos para copiar. O helper CUBE herdado captura direção para
texture fetch; não emula toda utilização aritmética do resultado. Investigar
se o jogo usa essa semântica além dos casos cobertos.

Aceite: shaders reconhecidos chegam a módulos/pipelines compatíveis e draws
executados; capturas com cena/estado reproduzível comparadas ao PC D3D12 e Xenos.
SPIR-V válido sozinho não comprova equivalência visual.

### F. Funcionalidades e desempenho

Depois de boot/imagem: testar entrada e controles, áudio/XMA, vídeos, loading,
saves/load, mundo aberto e transições. Revalidar hook XMA `0x826595B8` no fork
Switch. Medir CPU/GPU, memória, streaming AST e pacing antes de otimizar.
Resolução atual 1280x720; não importar remoção de efeitos/render-scale do PC
sem teste, pois há histórico de imagem corrompida. PGO e ordem de funções devem
ser gerados para Superman; não copiar perfis de NFSMW.

No console físico, registrar modelo/firmware, modo portátil/dock, forma de
lançamento e logs. Não há evidência atual de funcionamento no console.

## 4. Referências e comandos

Referência principal: https://github.com/StevensND/nfsmw-nx . A comparação recente
usou `df2de32ee569873062b8f8d1da8ad0b8d0a90a5d`; origem vendorizada e versões
exatas estão em `docs/provenance.json`. Não confundir as duas revisões.

Testes sem jogo:

```bash
python -m unittest discover -s tests -v
docker build -t superman-returns-nx-shaders -f shaders/Dockerfile .
docker run --rm --mount "type=bind,source=$PWD,target=/work" \
  -e DXC=/work/.tools/dxc/bin/dxc superman-returns-nx-shaders \
  bash /work/shaders/test_translator.sh
```

O segundo teste exige DXC Linux em `.tools/dxc/` e exports do SDK; preparar com
`python tools/fetch_thirdparty.py` e seguir `docs/shaders.md`.

Com biblioteca recebida/gerada:

```bash
docker run --rm --mount "type=bind,source=$PWD,target=/work" \
  superman-returns-nx-shaders bash /work/shaders/test_registry.sh \
  /work/out/shaders-cube-review/superman_returns_shaders.srsp
```

Probe Switch:

```bash
docker run --rm --mount "type=bind,source=$PWD,target=/project" \
  devkitpro/devkita64:latest bash /project/tools/switch/build-probe.sh
```

Para NRO completo, codegen, Mesa e pacote, seguir `docs/building.md`.
Teste local Windows: `tools/test-sudachi.ps1 -Nro CAMINHO -Seconds 30` e
`tools/test-ryujinx.ps1 -Nro CAMINHO -Seconds 30`. O usuário fará os testes que
não estiverem disponíveis na nuvem. Os emuladores instalados ficam em
`C:/Users/webpa/Music/sudachiemu.org-winpc-1-0-15` e
`C:/Users/webpa/Music/Ryujinx`.

## 5. Como entregar o PR

Entregar mudanças verificáveis por etapa. Informar problema, comportamento final,
arquivos principais, testes realmente executados e o que requer validação local.
Acrescentar roteiro de reprodução e logs esperados. Separar conserto de emulador
externo quando necessário. Não marcar tarefas como concluídas apenas por compilar.
Evitar reformatar o SDK inteiro e editar fontes PPC gerados; corrigir integração
ou gerador para que codegen possa ser repetido. Manter Xenos funcional como base
de comparação, respeitando as falhas existentes documentadas.

Prioridade do primeiro PR: reprodução/teste mínimo da falha gráfica e correção
com evidência, mais infraestrutura verificável necessária. Em paralelo apenas
conceitual, preparar o plano do renderer; não misturar uma portabilidade extensa
sem teste com uma correção pequena que já possa ser revisada e validada.
