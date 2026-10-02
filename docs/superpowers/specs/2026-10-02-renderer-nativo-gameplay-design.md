# Continuação do renderer nativo: gameplay no Nintendo Switch

Data: 2026-10-02.
Estado: direção aprovada em conversa; especificação escrita aguardando revisão.
Não há autorização de implementação decorrente apenas deste arquivo.

## Objetivo e escopo aprovado

Completar o renderer nativo do Superman Returns NX usando o projeto local
`C:/Users/Gusta/Documents/outros-projetos/superman_returns_recomp` como referência.
O resultado deve desenhar vídeos de abertura, título, menus, cidade, personagem,
capa e HUD no Switch físico, com áudio, entrada e encerramento funcionando.
Compilar um NRO ou apresentar uma imagem limpa não satisfaz esse objetivo.

A direção aprovada pelo usuário é reaproveitar os hooks D3D, layouts, captura de
estado e correções do PC; implementar desenhos, render targets, resolves e caches
em Vulkan; gerar SPIR-V e validar boot, menu e gameplay no console.

Mantêm-se resolução original, Xenos selecionável por execução e a meta de 30 FPS
nos clocks padrão, sem overclock. Esta meta precisa ser medida no Switch; os
resultados do PC não a demonstram. Cortes visuais exigem opção própria, comparação
e medição; não transplantar automaticamente o MSAA reduzido do PC.

Esta especificação amplia o escopo do documento de 2026-10-01 para os desenhos
reais e atualiza sua premissa sobre não usar o projeto de PC. O primeiro marco e
seus testes permanecem como base, com aceite em hardware ainda pendente.

## Bases e evidência

| Base local | Revisão examinada | Situação |
|---|---|---|
| Superman Returns NX | `5c4822bc4e9ad50b31e43db1b9cb835e41cb4b93` | Checkout limpo; parser, sincronização e clear Vulkan implementados; sem aceite no console |
| Superman Returns Recomp | `257feabc050e03c287fdf6238bf55876e5b59d81` | Checkout limpo; renderer D3D12, hooks e ferramentas de shaders disponíveis |

`checkpoint3.md` do PC registra gameplay sem draws pulados, capa corrigida e
30 FPS no laptop testado. Isso é evidência documental da referência, não uma
medição reproduzida nesta sessão. O README do PC ainda descreve problemas de uma
rodada anterior; ao adaptar, conferir o código e o checkpoint mais recente.

O PC possui corpus local em `artifacts/shaders/`, incluindo containers, HLSL,
DXIL e uma pasta SPIR-V. A presença de arquivos não comprova compatibilidade
com o ABI ou as capacidades Vulkan do Switch. O NX já possui leitor `.srsp`,
identificação e pipeline offline com DXC/spirv-val; preservar esse formato e sua
validação em vez de carregar a biblioteca DXIL `.srsl` do PC.

## Escolha de arquitetura

Adotar captura das chamadas D3D do jogo e execução Vulkan própria, preservando o
sistema gráfico nativo existente para MMIO, ring, interrupções e vblank. Adaptar
somente as partes independentes de D3D12 e reimplementar os recursos host em
Vulkan. Não introduzir execução Windows, tradutor em runtime no console ou
dependência do backend D3D12 na aplicação Switch.

Alternativas avaliadas: executar desenhos somente pelo PM4 reutilizaria mais do
parser NX, mas perderia a associação direta entre objetos e containers e exigiria
refazer a captura já resolvida no PC; copiar integralmente o renderer D3D12 criaria
uma dependência incompatível com Horizon. A adaptação por responsabilidades é a
continuação escolhida.

Fluxo: chamadas D3D guest → captura de estado e dados → fila ordenada de operações
nativas → comandos Vulkan → fences → apresentação pelo presenter do SDK.
O PM4 continua fornecendo o contrato de sincronização guest, associado ao trabalho
nativo correspondente. Não haverá fallback por draw para Xenos.

## Componentes e contratos

| Componente | Responsabilidade e fronteira |
|---|---|
| Perfil e hooks Superman | Endereços, assinaturas e offsets conferidos contra manifesto/código gerado do NX; associação shader-object/container; captura sem alterar a ABI PPC |
| Captura de estado | Snapshot de fetches, constantes, render state, viewport, scissor, streams, índices e superfícies; espelho PM4 do segmento D3D quando o estado não está no device |
| Fila e execução | Ordenar draw, clear, resolve e swap; copiar dados mutáveis; limitar memória e aplicar backpressure; rastrear conclusão por fence |
| Shaders e pipelines | Carregar `.srsp`, validar interface SPIR-V, criar layouts e pipelines Vulkan e contar ausência/incompatibilidade por identidade |
| Texturas e buffers | Endian, untile, formatos, mipmaps, dimensões, swizzles, uploads e invalidação de dados guest |
| Superfícies e resolves | Identidade EDRAM, alias, cor/HDR/profundidade, cópias, MSAA e dependências entre passes |
| Apresentação e sistema | Usar provider/presenter Vulkan existentes; MMIO, sincronização, interrupções, saída e diagnóstico |

Os nomes de arquivos serão definidos pelo plano conforme as dependências reais.
Manter unidades testáveis para formatos, captura, conversão de índices, espelho
PM4 e ordenação. Evitar transplantar o arquivo monolítico D3D12 do PC.

### Hooks, estado e dados mutáveis

Usar `game_profile.h`, `native_hooks.cpp`, `pm4_mirror.*`, `index_endian.h` e as
rotinas de captura do PC como referência. O cabeçalho antigo de `game_profile.h`
diz que nada foi confirmado, mas várias entradas possuem evidência e flags de
confirmação: avaliar cada entrada, não interpretar esse comentário como status
global. Verificar o SHA-256 do XEX e os símbolos recompilados antes de habilitar
os hooks no NX. Resolver sobreposição com `sr_shader_hooks.cpp`, `skip_intro.cpp`
e hooks de áudio: cada função guest deve ter um único ponto de interceptação.

Capturar os argumentos antes de executar a função original quando ela os altera,
e o objeto retornado depois da criação. Preservar a chamada original quando ela
produz ring, estados, memória ou efeitos que o jogo precisa. Identificar segmentos
do ring e ressincronizar o espelho em trocas; não reler o mesmo segmento como
operações novas. Constantes inline e literais carregados no ring fazem parte do
snapshot, mesmo quando o shadow state do device não é atualizado.

A fila não pode depender de ponteiros guest mutáveis depois da captura. Copiar
índices, vértices inline e regiões que o jogo pode sobrescrever antes da conclusão;
recursos persistentes devem ter versões e invalidação verificáveis. Incorporar a
detecção de atualização dos buffers pequenos da capa por frame, além de Unlock.
Testar reutilização de endereços e memória liberada.

### Ordenação, progresso e conclusão GPU

Draw/clear/resolve/swap pertencem a uma sequência monotônica. O parser pode
consumir um pacote sem trabalho gráfico somente quando o efeito correspondente
está definido; liberar uma fence guest dependente de GPU exige conclusão Vulkan
da sequência que ela cobre. Provar a associação entre comandos capturados e
pacotes PM4 com traces pequenos antes de habilitar consumo de draws nativos.
Não substituir essa associação por concluir toda a fila sem conferir limites.

Evitar execução duplicada: a operação observada pelo hook é emitida uma vez;
o PM4 correspondente é tratado como o mesmo trabalho, não como outro draw.
Efeitos que só aparecem no PM4 devem ser executados ou bloquear com diagnóstico.
Preservar endianness, predicação, buffers indiretos e writeback do parser NX.

Memexport, queries e coerência de memória guest exigem efeitos reais ou bloqueio
explícito. Dados de resolve lidos pela CPU precisam de readback, conversão e
conclusão antes da leitura dependente; manter uma imagem apenas na GPU não cumpre
esse contrato. Não liberar WAIT_REG_MEM por timeout, alterar SCRATCH para fabricar
conclusão ou portar atalhos de espera do PC sem justificativa.

O encerramento cancela produtores/esperas, interrompe o consumidor, drena o
trabalho submetido e destrói os recursos nessa ordem. Preservar as garantias do
marco 1 para falha parcial e device lost. Uma falha com trabalho pendente não
permite reciclar command buffers, descritores ou memória usados pela GPU.

### Shaders e ABI Vulkan

Conferir containers do corpus do PC, deduplicar por conteúdo e estágio e traduzir
offline usando correções do tradutor que se aplicam ao Superman. Registrar revisão
do tradutor, patches, opções DXC, hashes de entrada/saída e resultado de spirv-val.
Não copiar DXIL nem associar shader apenas pelo nome de arquivo ou hash sem
verificar a identidade esperada pelo leitor NX.

Definir uma interface única entre emissor e renderer para localizações de entradas
e saídas, constantes VS/PS, bools/loops, texturas, samplers e vertex ID. Refletir e
validar o SPIR-V gerado antes de criar layouts. Formato de constantes, padding,
componentes ausentes, offsets de fetch, normalize e endian devem corresponder à
captura. Conferir recursos exigidos contra o dispositivo Mesa/NVK efetivo; não
habilitar capacidades indisponíveis por pressuposto do PC.

Implementar inversão Y e profundidade em um único ponto consistente; o pipeline
offline NX atualmente usa `-fvk-invert-y`. Testar orientação, culling e viewport
sem aplicar a correção duas vezes. Quads indexados viram triângulos com winding,
base vertex e índices 16/32-bit corretos.

Shader ausente/incompatível gera contador e diagnóstico limitado por identidade.
Um draw comprovadamente sem efeitos de memória pode ser omitido durante a
migração, mas nenhum cenário com desenhos essenciais ausentes passa no aceite.
Falta de biblioteca em modo native é falha explícita de setup; ajustar o aviso
atual que diz que draws usarão Xenos para refletir o modo realmente selecionado.

### Recursos, imagem e apresentação

Reaproveitar a matemática de untile, endian e conversão da referência; substituir
DXGI/D3D12 por descrições próprias e VkFormat. Cobrir formatos e dimensões usados
no corpus de boot/menu/gameplay, com rejeição explícita dos demais. A identidade
da textura considera endereço, formato, dimensão, mipmaps e conteúdo/versionamento.

Implementar clear float4, retângulos, depth/stencil, viewport inteiro e faixa de
profundidade confirmados no PC. Render states incluem blend, máscaras de escrita,
alpha test, culling, depth/stencil e samplers. Materializar o estado relevante na
chave do pipeline; não depender de defaults host para estados guest.

Render targets devem preservar dependências e alias da EDRAM sem usar a emulação
Xenos como renderizador. Portar a correção de preto LDR reinterpretado como 7e3
e avaliar a unificação dos formatos guest 3/12, mantendo opção para comparação
pois ela modifica quantização. Resolves incluem limites, origem/destino, mip,
slice, clear associado e conversão de profundidade quando requerida.

Aplicar barreiras e layouts Vulkan entre attachment, sampling, transfer e host.
Apresentar a superfície final produzida pelo jogo usando o SDK, com lifetime
protegido até a conclusão do uso pelo presenter. A pintura UI não substitui a
contagem de swaps/draws concluídos. A rota de vídeos deve ser rastreada no
Superman: executar os uploads e draws resultantes do decoder guest, sem assumir
que o decoder WMV3 do NFSMW serve para os AST deste jogo.

### Memória e desempenho

Caches e fila terão limites configuráveis e contabilidade em bytes: CPU capturado,
uploads, imagens, buffers, pipelines e recursos aguardando descarte. Antes de
escolher limites padrão, medir a memória física disponível no console e os working
sets dos cenários. Atingir o limite causa descarte de recursos concluídos ou espera
cancelável por conclusão; nunca liberar recursos em uso ou crescer sem limite.

Começar pela correção. Medir custos de conversão/upload, criação de pipeline,
espera CPU, execução GPU e apresentação antes de otimizar. Prewarm e persistência
de pipelines incluem dispositivo/driver/ABI na identidade. Não transportar uma
cache D3D12 nem assumir a escala de timestamps usada por outra versão de NVK.

## Entregas e critérios

1. **Captura e shaders:** perfil/hook integrado, snapshots e espelho PM4 testados;
   corpus local traduzido e validado; layouts Vulkan coerentes com reflexão.
2. **Desenhos e recursos básicos:** draws indexados/inline, clear, texturas,
   constantes, profundidade e pipelines; traces comprovam ordenação e ausência
   de execução duplicada; NRO compila e diagnóstico identifica incompatibilidades.
3. **Imagem completa:** alias/HDR, resolves, leitura CPU e saída final; vídeos,
   título e menu visíveis no console com transições corretas.
4. **Gameplay e estabilidade:** cidade, Superman, capa animada, HUD e minimapa;
   entrada e áudio funcionando; memória limitada, sem bloqueios ignorados e saída
   segura. Registrar artefatos e observações de cada cenário.
5. **Desempenho:** medir gameplay parado e em movimento, com clocks reais e
   configuração registrados; corrigir gargalos demonstrados e repetir aceite de
   imagem quando a otimização modificar o resultado.

Essas entregas são dependentes e compõem um único objetivo. Uma entrega parcial
nunca será relatada como conclusão do renderer.

## Validação e conclusão

Testes host cobrem casos reais de formatos/endian/tiling, índices e quads,
snapshots mutáveis, constantes inline, ressincronização do espelho, associação
PM4/operações e fences pendentes/canceladas. Reexecutar os testes de parser,
lifecycle, shader safety, identificação do pack e ferramentas já existentes.
Usar instrumentação/sanitizers disponíveis, registrando suas limitações.

Verificação de build inclui cross-compilation do NRO, link sem dependências
D3D12/Windows e hashes do NRO, ELF, configuração e pack associados ao mesmo build.
Manter builds anteriores. O usuário abre o NRO e habilita FTP no console; testar
hardware não é substituído pelo Sudachi ou por testes com callbacks falsos.

No console, testar boot com vídeos e com skip-intro, título/menu, entrada na cidade,
movimento/capa e retorno/saída. Cada cenário de gameplay exige uma janela de 60 s,
depois uma sessão contínua de pelo menos 10 min para estabilidade, crescimento
de memória, áudio e shutdown. Capturas equivalentes do Xbox 360 são a referência
visual; a referência PC serve para diagnóstico e regressões, com suas diferenças
documentadas. Comparar também uma execução separada Xenos para progresso/áudio.

Aceite funcional exige zero erros de parser ou Vulkan, ausência de sincronização
fabricada, cobertura dos draws essenciais, transições corretas, áudio/entrada e
shutdown completo. Reportar qualquer defeito visual remanescente por cenário.

Aceite da meta de desempenho exige mediana de intervalo dos swaps concluídos
compatível com 30 FPS (até 33,4 ms), p95 até 35 ms e menos de 1% acima de 50 ms
em cada janela de gameplay definida, sem incluir menus ou carregamento. Registrar
também tempos de apresentação para distinguir produção de quadro e exibição.
Se o objetivo funcional for atingido mas essa meta falhar, relatar explicitamente
a parte concluída e o trabalho de desempenho restante.

## Proveniência e exclusões

Preservar avisos e revisões dos arquivos adaptados e atualizar
`THIRD_PARTY_NOTICES.md`. A referência PC marca trechos vindos do native-kit como
sem licença de kit identificada naquela revisão e conserva avisos BSD dos trechos
derivados. Não atribuir uma licença nova a esses trechos por suposição: conferir
a proveniência antes de copiar; quando não for demonstrável, implementar o
comportamento a partir dos contratos guest e SDK com código próprio.

Containers, shaders traduzidos, C++ gerado, jogo e dumps ficam locais/ignorados.
Não publicar assets, instalador, release ou alterar o projeto de PC nesta entrega.
Não incluir otimizações de cena Conan/NFSMW, overclock, redução de resolução,
tradução de shaders em runtime no Switch ou remoção de Xenos.

## Próxima etapa

Após o usuário revisar esta especificação escrita, criar o plano detalhado com
`superpowers:writing-plans`: dependências, arquivos, testes, comandos de build,
marcos de console e método de execução. O plano será apresentado para revisão
antes de implementar. A aprovação da direção em conversa não substitui essas
revisões requeridas pela skill de brainstorming.
