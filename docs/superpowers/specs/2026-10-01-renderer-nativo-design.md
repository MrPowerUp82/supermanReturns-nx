# Renderer Vulkan nativo para Superman Returns NX

Data: 2026-10-01. Base explorada: `1fa25c1`, com alterações locais do pack.
Estado: desenho em conversa e especificação escrita aprovados pelo usuário em
2026-10-01; plano de implementação sujeito a revisão antes da execução.

## Objetivo e escolhas aprovadas

Adaptar o renderer nativo do nfsmw-nx para executar Superman Returns no Switch
físico, em hbmenu full, usando shaders pré-compilados. O sistema consome os
comandos PM4 produzidos pelo D3D do jogo e gera trabalho Vulkan diretamente.

A meta final é 30 FPS estáveis nos clocks padrão, sem overclock. Isso é uma meta
de validação, não uma previsão de desempenho. Cortes visuais pequenos precisam
ser medidos e desligáveis; redução de resolução interna fica fora do escopo.
O Xenos permanece selecionável para diagnóstico e comparação.

O desenvolvimento do jogo e a validação de imagem/desempenho acontecem no Switch.
Compilação, análise de código, tradução offline e testes de host continuam no
computador; não se depende do projeto `superman_returns_recomp` ou de executar
o jogo no PC para aceitar um marco.

## Evidência atual e limites

- O pack atual contém 167 entradas: 62 VS e 105 PS.
- Em `out/console/latest-20261001-164409/logs/superman_returns_008.log`, o modo
  `identify` reconheceu 1/7 VS e 0/20 PS distintos; nenhum dos 12.251 draws
  observados tinha os dois shaders identificados. Nenhum draw usou o pack.
- O mesmo log registra chamada de `820F9C78` e contêineres ausentes da biblioteca
  observados pelos hooks D3DX. A rodada de captura já aconteceu. Antes de
  reconstruir o pack, é preciso conferir os arquivos gravados no SD e validá-los.
- A origem desses shaders ainda não está comprovada. A presença ou chamada de
  um helper isolado não prova compilação HLSL em runtime nem descompressão de AST.
- O caminho Xenos apresenta aproximadamente 0,5 FPS no logo EA e longas esperas
  por fences. Isso aponta para trabalho GPU ou submissão, mas não identifica o
  passe responsável. Ainda não existe uma decomposição confiável por timestamps.
- A imagem Xenos atual está corrompida. Ela serve como comparação de progresso
  e diagnóstico, não como referência suficiente de fidelidade visual.
- Reserva de espaço de endereçamento do processo não deve ser interpretada
  como consumo físico de memória. Medir uso real antes de dimensionar caches.

## Abordagem e decomposição

Escolha: port por marcos verificáveis, com ampliação do pack como frente
complementar. Medir todo o Xenos antes de começar e completar toda a biblioteca
antes do port foram considerados; ambos adiam a validação do novo sistema.

Esta especificação define a arquitetura geral e delimita a primeira entrega:
**sistema gráfico, consumo dos comandos, sincronização e apresentação limpa**.
Vídeo, draws, texturas e render targets completos são entregas posteriores.
Cada entrega posterior exige desenho e plano próprios baseados nas observações
do marco anterior. Não copiar todo o renderer da referência em uma única etapa.

## Arquitetura

### Seleção e ciclo de vida

Introduzir uma opção de app `sr_renderer = "native" | "xenos"`. Durante o
desenvolvimento, o padrão permanece `xenos`; cada build de teste registra o modo
selecionado. Valor inválido é erro explícito de configuração.

O modo nativo injeta uma implementação própria de `rex::system::IGraphicsSystem`
na configuração do runtime, antes do setup gráfico. A interface existente
oferece setup de apresentação, setup da GPU guest, callback de interrupção,
inicialização do ring, writeback do ponteiro de leitura e shutdown.

A escolha vale por execução. Não alternar sistemas em runtime e não fazer
fallback por draw do nativo para o Xenos. Falha de inicialização nativa é
registrada e encerra a execução; selecionar Xenos permite outra execução.

### Componentes e responsabilidades

| Componente proposto | Responsabilidade | Dependências |
|---|---|---|
| `sr_native_system` | Implementar `IGraphicsSystem`, MMIO, workers, callbacks e shutdown | SDK, runtime, sistema de apresentação |
| `sr_native_ring` | Ler PM4, validar pacotes, manter registradores e progresso | Memória guest e serviços do sistema nativo |
| `sr_native_present` | Recursos Vulkan, submissão, fences e apresentação | Provider/presenter Vulkan do SDK |
| Biblioteca/registro de shaders existente | Carregar e validar `.srsp`, mapear identidade | Pack local e hooks somente de observação |
| Draws, texturas, destinos e vídeo futuros | Converter estados do jogo em trabalho Vulkan | Ring, shaders e apresentação |

São fronteiras de responsabilidade, não uma exigência de criar módulos vazios
para etapas futuras. Reaproveitar provider/presenter e biblioteca existentes;
evitar um segundo parser do formato `.srsp`.

Fluxo principal: jogo PPC → D3D do Superman → ring PM4 na memória guest → leitura
e atualização de estado nativas → trabalho Vulkan → apresentação no Switch.
Writeback, SCRATCH e interrupções retornam progresso ao jogo conforme a operação
efetivamente processada e a semântica do comando.

### Adaptação da referência

Base local: `.tools/nfsmw-reference/`, revisão `df2de32`; preservar a proveniência
exata ao importar código. Reaproveitar as partes de sistema, ring e apresentação
de `nfsmw_nativo_sistema`, extraindo somente as dependências necessárias.

Não transplantar endereços PPC, layouts de objetos D3D, hooks de desenho,
atalhos de FlushState, funções de cena ou otimizações de passes do NFSMW sem
confirmar os equivalentes no Superman. O primeiro marco não depende de localizar
os construtores de shaders ou de instalar hooks que alterem o D3D do jogo.

As licenças raiz dos dois checkouts contêm GPL versão 3. Preservar também os
avisos e licenças específicos dos arquivos importados e atualizar
`THIRD_PARTY_NOTICES.md` na implementação. Dados derivados do jogo não entram
no repositório ou em artefatos publicados.

## Primeira entrega: comportamento e diagnóstico

1. Inicializar apresentação e MMIO usando a interface existente; iniciar workers
   somente depois de validar os recursos necessários.
2. Consumir o ring principal e buffers indiretos, respeitando endianness, tamanho,
   wrap, predicação aplicável e limites da memória guest. Escritas de registradores
   preservam efeitos observáveis pelo jogo.
3. Implementar writeback, SCRATCH, interrupções e vblank com semântica confirmada
   no SDK e no fluxo observado do Superman. Distinguir consumo de um pacote de
   conclusão do trabalho GPU.
4. Apresentar uma tela limpa por uma rota Vulkan explícita. Desenhos do jogo são
   contabilizados como omitidos neste marco. Comandos de transferência/resolve
   que afetem memória guest não podem ser tratados como draws descartáveis.
5. Encerrar workers antes de liberar memória e recursos Vulkan; shutdown deve ser
   seguro tanto após setup completo quanto após falha parcial.

O parser classifica comandos em suportados, omitidos deliberadamente para o
marco ou bloqueadores. Registrar opcode, localização e motivo, limitando repetição.
Um comando sem semântica confirmada que afete sincronização ou memória bloqueia
o marco, em vez de ser aceito silenciosamente.

Uma espera não satisfeita mantém sua condição original, permite cancelamento
no shutdown e produz diagnóstico periódico. Não alterar valores guest para
simular conclusão. Interrupções ou fences dependentes de desenhos omitidos
exigem justificar a semântica antes de liberá-los; não inferir sucesso pelo
simples avanço do ponteiro do ring.

Relatórios por intervalo incluem pacotes e opcodes, indiretos, posição de leitura,
writebacks, interrupções, vblanks, quadros apresentados, draws omitidos, esperas
pendentes e falhas Vulkan. Registrar identificação do build e modo selecionado.

## Shaders e etapas posteriores

Conferir e baixar os contêineres capturados, deduplicar, traduzir, validar SPIR-V
e reconstruir o pack local com identificação reproduzível. Separar entrada válida,
shader identificado e draw realmente desenhado nos contadores.

Reutilizar a identificação existente; quando o patch do D3D impedir identificação
segura, associar objeto de shader ao contêiner original por hooks confirmados
no Superman. Associações ambíguas não são aceitas como correspondências.

No renderer completo, draw com shader ausente ou incompatível é pulado e contado
com motivo. Isso permite investigação, mas uma cena com desenhos essenciais
ausentes não passa no aceite visual. Captura do logo não comprova cobertura
do menu, da cidade ou de gameplay.

Ordem posterior: vídeo EA correto → draws do pack e recursos necessários → menu
correto → cidade/gameplay → otimizações medidas. A rota de vídeo será definida
após confirmar como o Superman decodifica e desenha `DATA/fmvlegal.AST`;
não assumir que o decoder WMV3 do NFSMW é compatível.

## Validação

### Aceite da primeira entrega

- NRO compila e inicia em modo full no Switch físico com `sr_renderer = "native"`.
- Logs confirmam seleção nativa, consumo do ring, progresso observável do jogo
  e apresentação Vulkan; apresentar uma tela independentemente do guest não basta.
- Na mesma janela de teste de 60 segundos usada para o boot Xenos, não há crash
  ou bloqueio novo. Confirmar progresso guest por eventos identificados no log
  de referência, avanço dos comandos e/ou continuidade do áudio.
- Draws omitidos são contados. Não existem erros de parser, comandos bloqueadores
  ignorados ou conclusão de sincronização fabricada.
- Saída solicitada pelo usuário encerra a aplicação sem workers presos.
- Outra execução com `sr_renderer = "xenos"` preserva o comportamento anterior.

Testes de host cobrem casos relevantes do parser (pacotes truncados, wrap,
indiretos inválidos) e sincronização; eles não substituem o aceite no console.
Cada rodada muda uma coisa por vez e conserva o NRO e configuração anteriores.
O usuário inicia o NRO e disponibiliza FTP; logs e símbolos ficam associados ao
build exato. Transferências preservam arquivos e configuração alheios ao teste.

### Aceite visual e desempenho das entregas seguintes

Usar capturas equivalentes do Xbox 360 como referência visual. Xenos pode ajudar
no diagnóstico, mas sua imagem corrompida não é o padrão de correção.

Medir quadros apresentados, mediana e percentis do tempo por quadro, frequência
de quadros acima de 33,3 ms, CPU ativa versus espera, tempo GPU por categoria,
memória efetivamente usada e cobertura dos draws. Registrar cena, clocks reais,
configuração, duração e build. Boot/logo não demonstram gameplay a 30 FPS.

Calibrar timestamps da GPU com a versão Mesa/NVK usada e registrar a escala.
A referência usa fator 1,627; não fixá-lo sem confirmar no ambiente atual.
Fences medidas na CPU não são automaticamente tempo de execução GPU.
Comparações entre renderers exigem execuções distintas na mesma cena; ajustes
desligáveis dentro do nativo podem usar A/B na mesma sessão e controle de ruído.

Caches futuros terão limite e política de descarte definidos a partir do uso
real no console. Não copiar o orçamento do NFSMW por suposição.

## Riscos e pontos de investigação

A versão D3D e os shaders do Superman diferem da referência. Antes dos marcos
de desenho, confirmar construtores, fetch patches, layout do dispositivo e
compatibilidade da interface de constantes/texturas dos shaders com o renderer.
No primeiro marco, a prioridade é descobrir quais comandos de boot dependem
de efeitos GPU ainda ausentes e transformar essas dependências em diagnóstico.

A iteração depende de testes manuais no console. A apresentação limpa prova
somente o primeiro marco, não correção de imagem ou ganho final de desempenho.
Questões de vídeo, cobertura completa e orçamento de caches serão resolvidas
nos desenhos posteriores, sem impedir a implementação do sistema inicial.

## Próxima etapa

Após revisão e aprovação desta especificação escrita, criar o plano de
implementação da primeira entrega com a skill `writing-plans`. O plano deve
listar importações/adaptações, dependências, testes e builds de console e ser
revisado antes de escolher o método de execução. Esta especificação não autoriza
iniciar implementação antes dessas etapas.
