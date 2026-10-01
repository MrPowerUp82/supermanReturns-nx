# Checkpoint 5 — primeira rota de draw com os shaders pré-compilados

Continuação de `checkpoint4.md` no checkout local
`C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx`, sobre `1fa25c1`.
Alterações ainda locais, sem commit.

## O que foi feito

- Rota de draw com o pack dentro do backend Xenos Vulkan, com fallback por draw.
  Detalhes e limitações em `docs/shaders.md`, seção "Draws com o pack dentro do
  backend Xenos". Arquivos novos: `sdk/include/rex/graphics/pack_shader_sources.h`,
  `sdk/src/graphics/pack_shader_sources.cpp` (rexcore),
  `sdk/src/graphics/vulkan/pack_shaders.{h,cpp}`,
  `sdk/src/graphics/vulkan/command_processor_pack.cpp`,
  `shaders/test_pack_identify.{cpp,sh}` e `shaders/test_stubs/`.
  Alterados: `command_processor.{h,cpp}` (ciclo de vida e desvio no `IssueDraw`
  depois do update de render targets), `pipeline_cache.{h,cpp}`
  (`ConfigurePackPipeline`), `texture_cache.{h,cpp}` (view 2D não-array),
  `switch_perf.cpp` (contadores 33-35), app (`sr_shader_hooks.cpp` registra o pack;
  `superman_returns_app.h` liga `vulkan_native_shader_features` com `draw`).
- Cvar `pack_shaders`: `identify` (padrão) conta cobertura e desenha com Xenos;
  `draw` desenha com o pack o que for suportado; `off` ignora.
- Teste de host passou (sintético + 167 contêineres distintos: 165 identificados
  com a própria tradução, 2 PS ambíguos ficam no Xenos). O teste pegou um defeito
  real (fetches reordenados pelo D3D) que foi corrigido antes do build.
- NRO compilado no volume `superman-returns-nx-check` sem erros novos.

## Achados da investigação

- No console o caminho de render target é host RT (não FSI); o log não mostra
  fallback para FSI.
- Esperas de fence ~989 ms/s: o tempo está na GPU (ou na cadeia de submissão), não
  na CPU do processador de comandos. Ainda não se sabe qual parte da GPU custa.
- O logo EA vem de `DATA\fmvlegal.AST` (FMV). Falta confirmar se o vídeo é
  decodificado em software e desenhado com shaders do pack.
- Endereços com assinatura de contêiner no PPC: `823AE558`, `823FC5A8`, `82383AA8`,
  `823DBB38`, `823B2530`, `823B2C20`, `828439F0`, `820F9C78`. Nenhum foi confirmado
  como construtor D3D; `828439F0` parece converter contêineres (códigos de erro
  4801/3500). A rota nova não depende deles.

## Teste físico preparado

Enviados ao SD via FTP (`192.168.100.37:5000`, `/switch/superman-returns-nx/`),
como arquivos novos: `superman_returns-pack-test.nro` (menu "Superman Returns NX Pack
test", 59.955.397 bytes, SHA-256
`cb50bdaddda2d355f82b67d853f886c9cbb52930b6f08ed47c276fc423e0a861`) e
`superman_returns_shaders.srsp` (o pack de `out/shaders-cube-review`). ELF em
`out/console/symbols/superman_returns-pack-test.elf`. Backup do TOML e do
`rex_crash.log` antigo em `out/console/2026-10-01-before-pack-test/`. O TOML do SD
ainda não tem `pack_shaders` (logo, `identify`). Os outros NRO no SD agora também
encontram o `.srsp` e só fazem a identificação antiga de recursos.

Sequência combinada:
1. Rodada `identify`: abrir o Pack test no hbmenu full, ~60 s no logo EA, sair.
   Coletar logs (`superman_returns_00N.log`, `logs/rex/rex_perfil.log`): linhas
   `Shader pack`, avisos de shader não encontrado, "shaders precompilados".
2. Rodada `draw`: acrescentar `pack_shaders = "draw"` ao TOML do SD (preservando o
   resto), mesma cena, mesmo tempo. Comparar imagem, FPS, esperas de fence, draws
   desenhados com o pack e motivos de fallback; procurar erros de pipeline.
3. Restaurar o TOML (sem `pack_shaders`) depois do teste.

Registrar separadamente: shaders identificados, draws cobertos, draws desenhados
com o pack, imagem correta e desempenho. Não concluir ganho de FPS sem a comparação.

## Resultado da rodada identify (2026-10-01)

Logs em `out/console/pack-identify-result/`. O NRO iniciou normalmente, pack com
62 VS e 105 PS carregado. Na cena do logo EA: VS 1/7 e PS 0/20 microcódigos
distintos identificados; 0 de ~625 draws por 10 s cobertos (todos com shader fora
do pack); 0,5 FPS, como antes. A maioria dos shaders carregados tem comprimento
(9-207 palavras) que nenhuma entrada do pack tem; três VS de 24 palavras ficam a 2
palavras da entrada 49, um PS de 15 palavras a 1 palavra da entrada 111.
A diferença não é um deslocamento sistemático de tamanho. Os arquivos do jogo só
têm contêineres `0x102A11xx` (547 candidatos: baseworl 4, metropol 114, objtiv 402,
image.bin 27), então os shaders desta cena devem estar comprimidos dentro dos AST
ou embutidos como microcódigo cru (XEX/D3D).

Rodada seguinte preparada: TOML do SD com `dump_shaders =
"/switch/superman-returns-nx/shaderdump"` (ainda identify), para comparar o
microcódigo real com o pack e procurar sua origem nos arquivos. Restaurar o TOML do
backup depois.

## Rodada com dump de microcódigo: causa encontrada

`out/console/pack-dump-result/` (27 shaders em `shaderdump/`). Só 2 dos 27
microcódigos existem nos arquivos, ambos crus no XEX (`image.bin`, shaders internos
do D3D). Os outros 25 não aparecem nem em janelas de 3 palavras. O XEX contém o
compilador de efeitos/HLSL do D3DX (`ID3DXEffectCompiler`, HLSL com
`compile ps_3_0`): **o jogo compila a maioria dos shaders em tempo de execução**, e
esses contêineres nunca existem nos arquivos. O scanner offline nunca poderia achá-los.

`sub_820F9C78` é quem grava os contêineres compilados (stream em `r4+20`: +0 dados,
+4 posição, +8 capacidade, +12 bytes escritos; corpo primeiro, cabeçalho de 36 bytes
depois). Hook novo em `app/src/sr_shader_hooks.cpp`: ao retornar com sucesso, lê o
contêiner, loga se está na biblioteca e, com `sr_dump_shader_containers = true`,
grava em `shader_containers/<hash>.<v|p>.bin` ao lado do NRO.

NRO "Pack test" substituído no SD (SHA-256
`d3bfe4be68e76d5765ef47995433ce5f544474adda93eb320487c47adde05a73`, ELF
`out/console/symbols/superman_returns-pack-test2.elf`; o anterior virou
`-pack-test1.elf`). TOML do SD com `dump_shaders` e `sr_dump_shader_containers`.

Plano: coletar os contêineres compilados, concatená-los num arquivo e passá-lo como
entrada extra de `shaders/build_library.sh` (o scanner acha contêineres em qualquer
arquivo), gerar um pack novo e repetir identify/draw. Os contêineres são dados
derivados do jogo: ficam locais, como o pack.

## Correção: não é compilação em tempo de execução

Rodada seguinte (`out/console/pack-capture-result/`): o hook em `sub_820F9C78`
estava linkado (conferido com `nm` no ELF) e nunca foi chamado. O compilador HLSL do
XEX parece servir só a um efeito de depuração. Os AST são arquivos EA `BGFA1.05`;
`preload.AST` é quase todo de alta entropia. A hipótese atual: os contêineres estão
comprimidos nos AST e são descomprimidos na memória. Os 18 contêineres rejeitados do
`image.bin` são VS internos do D3D sem tabela de constantes; os 2 microcódigos
encontrados no XEX estão fora de contêineres.

Captura generalizada (`app/src/sr_shader_hooks.cpp`): hooks só de leitura em
`82383AA8` (descritor RenderWare tipo 15), `820F9C78` e nos helpers D3DX `823AE558`,
`823B2530`, `823B2C20` (r3), `823FC5A8`, `823DBB38` (r4); registram a primeira
chamada de cada um e cada contêiner distinto (e gravam com
`sr_dump_shader_containers`). NRO "Pack test" no SD: SHA-256
`60fb92d2660401f2361321db860bd66a77c36ab8771ad1ba289c2c6a5bb972bd`, ELF
`superman_returns-pack-test3.elf`. Se nenhum hook capturar, o próximo passo é
decodificar os BGFA offline ou localizar a criação dos shaders no D3D.

## Próximos passos prováveis

- Se VS não forem identificados (IM_LOAD_IMMEDIATE com saídas anuladas pelo D3D),
  confirmar os construtores D3D e associar por objeto, como no nfsmw-nx.
- Geometry shader próprio para retângulos (vídeo e 2D costumam usar), sinais de
  textura e gamma conforme os motivos de fallback medidos.
- Medir tempo de GPU por categoria (timestamps) para saber o que ocupa os ~2 s por
  quadro antes de prometer FPS.
