# Biblioteca de shaders do Superman Returns

`shaders/build_library.sh` gera uma biblioteca SPIR-V a partir da cópia própria
do jogo. O resultado é preparação para um renderizador nativo Vulkan. O NRO
atual carrega essa biblioteca para identificar recursos do jogo; os draws ainda
usam o backend genérico Xenos.

## Reprodução com Docker

Exporte primeiro as dependências do SDK com `python tools/fetch_thirdparty.py`.
O tradutor usa os headers de fmt e xxHash em `sdk/thirdparty/`. Extraia o pacote
Linux do DXC em `.tools/dxc/`, preservando `bin/` e `lib/` juntos. O pacote usado
localmente foi `linux_dxc_2026_09_28.x86_x64.tar.gz`, release v1.9.2609 do
[DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2609).
O SHA-256 do arquivo baixado está em [provenance.json](provenance.json).

No PowerShell, a partir da raiz deste projeto:

```powershell
docker build -t superman-returns-nx-shaders -f shaders/Dockerfile .
$shaderRepo = (Get-Location).Path
$shaderGame = (Resolve-Path ../superman_returns_recomp/game).Path
docker run --rm `
  --mount "type=bind,source=$shaderRepo,target=/work" `
  --mount "type=bind,source=$shaderGame,target=/game,readonly" `
  -e DXC=/work/.tools/dxc/bin/dxc `
  superman-returns-nx-shaders `
  bash /work/shaders/build_library.sh /work/out/shaders-rebuild /game
```

O destino precisa ser novo: o script recusa uma pasta existente. Para incluir
os nove contêineres extras da rodada local, monte também o arquivo descompactado
`../superman_returns_recomp/logs/image.bin` como `/image.bin`, somente para leitura,
e acrescente `/image.bin` depois de `/game` no comando. Esse dump é uma entrada
local opcional; este projeto ainda não fornece um comando reproduzível para
extraí-lo do XEX. Sem ele, a cópia de jogo verificada fornece 520 contêineres.

O script compila o scanner, o tradutor e o packer; valida limites, tabelas de
constantes e microcódigo dos contêineres `0x102A1100/01`; traduz para HLSL;
compila `vs_6_6`/`ps_6_6` para Vulkan 1.2; executa `spirv-val` com
`--scalar-block-layout`; empacota e verifica o reencontro de todos os contêineres.
O vertex shader usa `-fvk-invert-y`; ambos os estágios usam `-fvk-use-dx-layout`.
O formato `SRSSPV` usa xxHash3 do contêiner original para procurar o SPIR-V e
recusa a assinatura do pacote de NFSMW.

Os resultados incluem `containers/provenance.tsv`, HLSL, SPIR-V, logs e o
`.srsp` com seu SHA-256. Na rodada anterior, apenas a biblioteca, os logs e uma
cópia de `provenance.tsv` foram preservados em `out/shaders/`. `out/` e `.tools/`
são ignorados pelo Git; os resultados derivados do jogo ficam locais.

## Ferramentas verificadas nesta máquina

| Componente | Versão local |
|---|---|
| Base Docker | `debian:trixie-slim` |
| g++ | Debian 14.2.0-19 |
| glibc | 2.41-12+deb13u4 |
| SPIRV-Tools | v2025.1, data informada 2025-03-18 |
| DXC | release v1.9.2609; `--version`: 1.9.2609.5, commit 01b62ad4 |

A imagem usa trixie porque o DXC Linux usado exige glibc 2.38 ou posterior.
O Dockerfile instala pacotes via apt e a tag da base é móvel: uma reconstrução
futura pode usar versões diferentes. O ID completo da imagem local testada foi
registrado em `provenance.json`; ele identifica a imagem local, sem garantir
que ela esteja disponível num registry.

## Alterações em relação à base NFSMW/XenosRecomp

- Constantes de pixel ampliadas de 224 para 256 `float4` (4.096 bytes), pois o
  jogo acessa c229 e acima. O futuro renderer deve enviar os 256 registradores.
- Declarados os 128 temporários Xenos; há shaders do jogo que usam até r51.
- Arrays de constantes booleanas mapeiam todos os elementos, incluindo b0–b3
  de `RwSkinUsedWeights`.
- `CUBE` usa o leitor normal de operandos para preservar constantes, swizzle,
  absoluto, negação e índices relativos. A entrada Xenos `Z_XY` é reorganizada
  para XYZ antes do helper de cubemap; a máscara de escrita também é respeitada.
  A referência para a ordem dos componentes é
  [Xenia, ProcessVectorAluOperation](https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/spirv_shader_translator_alu.cc).
  O helper herdado captura a direção para o texture fetch, sem emular todo uso
  aritmético possível do resultado CUBE. Testes não comprovam imagem correta.
- `NORMAL0` acrescentado à lista de interpoladores de ambos os estágios.
- O packer aceita a parte física do formato de 2008 sem exigir múltiplo de 12:
  ela também contém constantes imediatas depois do microcódigo.

O macro `NFSMW_RECOMP` seleciona o caminho Vulkan herdado. As reescritas HLSL
de sombra PCF, sombra por mínimo e blur específicas de NFSMW não são aplicadas.
A origem é a revisão de NFSMW registrada em `provenance.json`; XenosRecomp
mantém o aviso MIT em `shaders/LICENSE.md` e sua documentação original em
`shaders/README.upstream.md`.

## Revisão de CUBE e nova biblioteca (2026-10-01)

A revisão do `nfsmw-nx` (commit
`df2de32ee569873062b8f8d1da8ad0b8d0a90a5d`) reforçou que compilação e validação
SPIR-V não bastam para confirmar semântica. A implementação anterior de CUBE
passava o registrador inteiro diretamente ao helper. Isso ignorava a seleção
dos componentes e os modificadores, inclusive no caminho adicionado para
constantes do Superman.

O corpus regenerado contém 173 chamadas CUBE em 146 contêineres. Treze chamadas
têm fontes diferentes de um temporário com o swizzle padrão `zzxy`, incluindo
`c250.zzzw`, `c247.xxxy`, `r0.wwyz` e `r6.xxzy`. Elas agora usam os componentes
codificados na instrução original.

`shaders/test_translator.sh` testa oito casos sintéticos sem dados do jogo:
temporário, absoluto/negado, constante literal, índices relativos por a0 e aL,
rejeição de constante relativa não declarada, escrita parcial e swizzle distinto.
O HLSL gerado também é compilado com DXC e validado por `spirv-val`:

```powershell
docker run --rm `
  --mount "type=bind,source=$((Get-Location).Path),target=/work" `
  -e DXC=/work/.tools/dxc/bin/dxc superman-returns-nx-shaders `
  bash /work/shaders/test_translator.sh
```

Nova execução completa: `out/shaders-cube-review/`, com os contêineres, HLSL e
SPIR-V preservados. Os 529 traduziram, compilaram e passaram no `spirv-val`;
o packer confirmou o reencontro de todos, em 167 entradas únicas.
Biblioteca: 9.081.800 bytes, SHA-256
`26d2c05bb357507be0264da585f643e517695ffac66b72383d817db5036ce181`.
A biblioteca anterior em `out/shaders/` foi preservada para comparação.
Comparando os dois pacotes, os 167 contêineres originais são idênticos;
31 entradas SPIR-V de pixel mudaram e nenhum vertex shader mudou. Diferença
binária de SPIR-V não mede, por si só, diferença visual ou desempenho.

## Integração no NRO (2026-10-01)

O aplicativo carrega `superman_returns_shaders.srsp` ao lado do NRO, depois de
inicializar o log e antes da GPU. O carregador valida formato, checksums e estágios
SPIR-V. Arquivo ausente ou inválido produz um aviso e mantém o backend Xenos.
O Sudachi registrou `SR shader library: 167 entries loaded` na execução local.

`sr_shader_hooks.cpp` observa `sub_82383AA8`, verificada no corpo PPC gerado:
o argumento r4 é um descritor tipo 15, com ponteiro/tamanho do contêiner em +48/+52;
o retorno r3 é um recurso do engine. A identificação usa o contêiner completo
antes da função original. O registro invalida associações quando um endereço é
reutilizado com conteúdo desconhecido. A função original sempre é executada.
Este recurso ainda não é um objeto D3D nem um pipeline Vulkan.

`shaders/test_registry.sh` validou as 167 entradas, padding externo, rejeição de
truncamento/assinatura desconhecida, reutilização de endereços e recusa de reload
que invalidaria ponteiros. Para repetir no container de ferramentas:

```bash
bash /work/shaders/test_registry.sh /work/out/shaders-cube-review/superman_returns_shaders.srsp
```

Sem argumento, o mesmo teste usa uma biblioteca sintética (24 contêineres
fabricados, sem dados do jogo) e também verifica o carregador: pacote alterado,
de outro jogo, truncado, com bytes sobrando ou ausente é rejeitado. Roda em
qualquer ambiente com g++ 13+ e os headers de xxHash exportados:

```bash
bash shaders/test_registry.sh
```

O teste CUBE (`test_translator.sh`) também roda fora do Docker, num host com
glibc 2.38+: `DXC=$PWD/.tools/dxc/bin/dxc bash shaders/test_translator.sh`.

O pacote opcional inclui a biblioteca com:

```powershell
python tools/project.py package --shader-library out/shaders-cube-review/superman_returns_shaders.srsp
```

O pacote confere apenas tamanho e cabeçalho; a validação completa ocorre no NRO.

## Draws com o pack dentro do backend Xenos (2026-10-01)

Primeira ligação dos SPIR-V aos draws. Não é um renderer nativo: o D3D do jogo
continua escrevendo o ring PM4 e o processador de comandos Xenos continua dono de
render targets, texturas, resolves e processamento de primitivas. Só os shaders
mudam, e só nos draws que o pack consegue reproduzir; o resto continua no Xenos.

- O app registra as entradas do `.srsp` (`rex/graphics/pack_shader_sources.h`).
  O cvar `pack_shaders` escolhe `off`, `identify` (padrão: conta, desenha com Xenos)
  ou `draw`. Com `draw`, o app liga `vulkan_native_shader_features`, porque o SPIR-V
  declara Int64, endereços de buffer e arrays de descritores sem tamanho.
- Identificação no IM_LOAD (`sdk/src/graphics/vulkan/pack_shaders.cpp`, adaptada de
  `nfsmw_nativo_shaders.cpp`): pixel shader por microcódigo inteiro; vertex shader
  com os fetches declarados reduzidos ao que o D3D não altera (opcode, registradores,
  predicado) e ordenados, porque o D3D reescreve constante de fetch, formato, stride,
  offset e swizzle e pode reordenar os fetches. Contêineres com o mesmo microcódigo e
  SPIR-V diferente ficam com o Xenos.
- A entrada de vértices sai dos fetches já corrigidos pelo D3D: formato Vulkan,
  stride, offset e `g_InputRemap` por localização (lógica de `CalcularEntrada`). Os
  vértices usados pelo draw são copiados do guest com a troca de bytes do fetch
  constant; índices DMA são convertidos para a ordem do host (o VS Xenos fazia isso
  no shader).
- Pipeline próprio (`VulkanPipelineCache::ConfigurePackPipeline`): o estado fixo vem
  da mesma descrição do pipeline Xenos do draw; estágios, layout e entrada de
  vértices são do pack. Layout: sets 0-2 `Texture2D/3D/Cube[]`, set 3 samplers,
  índice = fetch constant (`PARTIALLY_BOUND`); set 4 com as constantes VS e PS
  (256 `float4` cada, direto dos registradores) e o bloco compartilhado (booleanos,
  alfa, `g_NdcScale/Offset` do viewport Xenos com Y negado por causa do
  `-fvk-invert-y`, remapeamentos). Especialização: UBO, função de alfa, R11G11B10.
- Volta para o Xenos, com contador por motivo: shader fora do pack, retângulos,
  pontos, quads e fans (precisam de geometry shader), memexport, posição pré-dividida
  ou 1/W, planos de recorte ou kill de vértice, render target gamma/7e3/16 bits fixo
  ou com expoente, PS que escreve profundidade, alpha-to-coverage, textura com sinal
  misto, bias, gamma ou expoente, textura 1D ou de vertex shader, índices convertidos
  pelo Xenos, Z de OpenGL ou invertido.

Teste sem jogo nem GPU: `bash shaders/test_pack_identify.sh` (sintético) ou com
`out/shaders-cube-review/containers` como argumento. Em 2026-10-01: VS corrigido e
reordenado identificado, entrada de vértices e stream separado conferidos, alterações
de ALU e de registrador rejeitadas; dos 167 contêineres distintos, 165 identificados
com a própria tradução e 2 PS deixados para o Xenos (mesmo microcódigo, SPIR-V
diferente). Isso não valida a imagem nem o comportamento no console.

Contadores no `rex_perfil.log` ("shaders precompilados") e resumo a cada 10 s no log
(`Shader pack (...)`: microcódigos identificados, draws cobertos, desenhados com o
pack, pipelines e motivos de volta ao Xenos).

## Renderização nativa pendente

Confirmar no executável do Superman os construtores D3D. O perfil do projeto PC em
`port/src/native_renderer/game_profile.h` ainda marca os candidatos de criação
de shaders como não confirmados; eles não devem ser tratados como hooks prontos.
A rota acima não usa esses hooks. Faltam ainda geometry shader próprio para
retângulos/quads, gamma e formatos especiais de render target, sinais de textura,
shaders sem CTAB e qualquer redução do custo de CPU do Xenos por draw.

O teste visual e o boot completo continuam pendentes. A biblioteca offline não
resolve a inicialização gráfica nem o tratamento de memória dos emuladores;
o estado atual desses testes está em [validation.md](validation.md).
