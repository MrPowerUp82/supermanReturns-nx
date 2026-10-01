# Plano em aberto: renderer nativo para o Superman Returns NX

Documento de passagem para terminar o plano em outra ferramenta (Codex). Saiu de uma
sessão de brainstorming em 2026-10-01. O entendimento abaixo ainda **não foi
confirmado** formalmente, e nenhum design foi aprovado. Nada aqui foi implementado.

Repositório: `C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx`
(base `1fa25c1`, alterações locais sem commit; ver `checkpoint5.md`).
Referência: https://github.com/StevensND/nfsmw-nx, cópia local em
`.tools/nfsmw-reference/` (commit `df2de32`).

## 1. Resumo do entendimento

- **O que construir:** um renderer nativo Vulkan para o Superman Returns NX, portado do
  nfsmw-nx (`app/src/nfsmw_nativo_*`, cerca de 35 mil linhas). Ele lê o ring PM4
  escrito pelo D3D do próprio jogo e desenha com os shaders pré-compilados do pack
  (`.srsp`). A emulação Xenos sai do caminho principal.
- **Por quê:** o caminho atual roda a ~0,5 FPS e é limitado pela GPU emulada (esperas
  de fence ~989 ms/s). No nfsmw-nx, essa troca levou o jogo de 1–3 para 30–35 FPS.
- **Para quem:** o port no Switch físico, rodando em modo full.
- **Meta:** 30 FPS estáveis com os clocks padrão, usando o maior clock permitido sem
  overclock (por exemplo GPU a 460,8 MHz no dock).
- **Ordem:** dois trabalhos em paralelo.
  - Port do renderer, começando por ring PM4, sincronização com o jogo,
    apresentação e vídeo. Draws com shader fora do pack ficam pulados e contados.
  - Completar o pack com os shaders que hoje faltam.
- **Qualidade de imagem:** cortes pequenos aceitos, como os do nfsmw-nx, desde que
  cada um seja medido e desligável por opção.
- **Desenvolvimento e testes:** direto no Switch. O caminho Xenos continua
  selecionável por opção.

## 2. Fora do escopo

- Overclock como meio de chegar à meta.
- Desenvolvimento ou validação no PC (o projeto `superman_returns_recomp` não é usado).
- Cortes visuais grandes, como reduzir a resolução interna.

## 3. Suposições (a confirmar)

1. O código do nfsmw-nx pode ser portado: os dois projetos são GPL-3. Manter o
   crédito e os avisos (`THIRD_PARTY_NOTICES.md`).
2. As partes específicas do NFSMW começam desligadas: funções nativas do jogo, cortes
   de passes, sombra por mínimo, LOD de sombras, prewarm de pipelines atrelado a
   endereços do NFSMW. Só voltam reescritas para o Superman e medidas.
3. No modo nativo, draws com shader fora do pack são pulados e contados. Não há
   fallback para o Xenos dentro do mesmo modo.
4. A medição segue `docs/measuring.md` da referência:
   - timestamps da GPU corrigidos pelo fator 1,627 do NVK no Switch;
   - `BOTTOM_OF_PIPE` nos dois lados de cada medição;
   - A/B na mesma cena;
   - mediana dos quadros.
5. Logs, pack e contêineres extraídos do jogo ficam só locais e nunca são publicados.
6. Cada build de teste muda uma coisa de cada vez e traz diagnóstico suficiente para
   uma única rodada no console. Cada rodada depende do usuário abrir o NRO e liberar
   o FTP (`192.168.100.37:5000`, `/switch/superman-returns-nx/`).

## 4. Registro de decisões

| # | Decisão | Alternativas consideradas | Motivo |
|---|---|---|---|
| D1 | Seguir o caminho do nfsmw-nx (renderer nativo, imagem do Xbox 360) | FPS rápido com perdas; medir primeiro e decidir depois | Escolha do usuário; é o caminho já comprovado no mesmo hardware |
| D2 | Portar o renderer do nfsmw-nx e adaptar | Escrever um renderer próprio menor; híbrido só com peças comprovadas | Escolha do usuário; caminho mais rápido até algo funcional |
| D3 | Port e cobertura de shaders em paralelo | Completar o pack antes de portar | Escolha do usuário; o início do port (ring, sincronização, apresentação, vídeo) não depende do pack |
| D4 | Meta de 30 FPS estáveis | Primeiro marco de 15–20 FPS; sem meta por enquanto | Escolha do usuário |
| D5 | Clocks padrão, o maior permitido sem overclock | Só o modo portátil; overclock permitido | Escolha do usuário |
| D6 | Cortes visuais pequenos, medidos e desligáveis | Fidelidade total; qualquer corte | Escolha do usuário; igual à política do nfsmw-nx |
| D7 | Desenvolvimento direto no Switch | PC primeiro (como o nfsmw-nx); híbrido | Escolha do usuário. Custo: ciclo lento, cada iteração depende de uma rodada no console |
| D8 | Xenos selecionável por opção | Remover o Xenos; remover depois | Escolha do usuário; serve de comparação e diagnóstico |

## 5. O que já se sabe (fatos medidos)

- O console usa render targets normais da GPU, não o modo por interlock (FSI).
- O caminho atual registra ~125 draws e ~26 resolves por quadro, ~129 transferências de
  render target e ~24 texturas carregadas por quadro.
- O logo EA é um vídeo em `DATA\fmvlegal.AST`. Ainda não se sabe como ele é desenhado.
- **Cobertura do pack:** em 2026-10-01 o `.srsp` tinha 167 contêineres distintos (62 VS,
  105 PS). O teste de host passou: 165 identificados com a própria tradução e 2 PS
  ambíguos. Na cena do logo, porém, 0 de 27 shaders carregados pelo jogo estão no pack.
- **Origem dos shaders que faltam:**
  - 25 dos 27 não existem nos arquivos do jogo, nem em pedaços de 12 bytes.
  - Os outros 2 são microcódigo cru do D3D dentro do XEX.
  - Os `.AST` são arquivos EA `BGFA1.05`; o `preload.AST` é quase todo comprimido.
  - Hipótese: os shaders estão comprimidos nos AST.
  - A hipótese de compilação em tempo de execução foi descartada: o hook no gravador
    de contêineres `sub_820F9C78` estava linkado e nunca foi chamado.
- **Pendente:** um NRO de captura ("Pack test", SHA-256 `60fb92d2…72bd`) está no SD.
  Ele registra a primeira chamada de cada hook e grava em `shader_containers/` cada
  contêiner que o jogo entregar. Os pontos observados são `82383AA8`, `820F9C78`,
  `823AE558`, `823B2530`, `823B2C20`, `823FC5A8` e `823DBB38`. A rodada no console
  ainda não foi feita.
- **Já implementado nesta sessão** (base possível para reaproveitar; ver
  `docs/shaders.md`):
  - identificação do pack no IM_LOAD, com VS corrigido pelo D3D e fetches
    reordenados;
  - entrada de vértices a partir dos fetches corrigidos;
  - pipeline do pack dentro do backend Xenos;
  - contadores no `rex_perfil.log`;
  - teste de host `shaders/test_pack_identify.sh`.

## 6. Perguntas em aberto

1. Os shaders que faltam saem das `.AST` (decodificar o BGFA e a compressão usada) ou da
   captura em tempo de execução? A rodada pendente responde em parte.
2. Onde a versão do D3D do Superman (contêiner 2008) difere da do NFSMW (2005) nos
   pontos que o renderer nativo usa?
   - construtores de shader (`sub_8259BC90` e `sub_8259C038` no NFSMW);
   - o "patcher" de fetches;
   - FlushState;
   - o IM_LOAD_IMMEDIATE com saídas anuladas;
   - a espera `WAIT_REG_MEM` / `SCRATCH_REG`.

   Nenhum endereço do NFSMW vale para o Superman.
3. O vídeo do logo usa um decodificador de software do jogo (o NFSMW tem rota WMV3
   própria) ou é desenhado por shaders do pack?
4. Qual o custo de GPU por categoria no caminho atual? Ainda não há timestamps. Seria
   uma base de comparação para o nativo.
5. Quais passes e funções do Superman equivalem às otimizações do nfsmw-nx? Exemplos:
   sombras, reflexos, pós-processamento, material, matrizes e visibilidade.
6. Como validar a imagem sem desenvolvimento no PC? Possibilidades: capturas de tela no
   console, ou comparação com o modo Xenos selecionável.

## 7. Abordagens possíveis (não validadas)

Esboço para discussão, ainda sem aprovação.

**A. Port faseado por marcos verificáveis no console** (candidata principal)
1. Sistema gráfico próprio (`IGraphicsSystem`) com leitura do ring, sincronização
   (`SCRATCH_REG`, interrupções, vblank) e apresentação de uma tela limpa. Critério: o
   jogo avança no boot como com o Xenos, sem travar.
2. Rota de vídeo do logo EA. Critério: vídeo visível e correto.
3. Draws com shaders do pack, pulando e contando os desconhecidos. Critério:
   contadores coerentes.
4. Render targets sem EDRAM, resolves e texturas. Critério: menu correto.
5. Cidade e gameplay, seguidos de otimização de GPU e CPU até 30 FPS, com cortes
   medidos.

**B. Medir antes de portar:** adicionar timestamps por categoria ao caminho Xenos para
ter uma linha de base. É barato e responde à pergunta 4, mas atrasa o port em uma ou
duas rodadas.

**C. Completar o pack primeiro, offline:** decodificar o BGFA, regenerar o `.srsp` e só
então começar o port. Reduz o tempo com draws pulados, mas atrasa o começo do port.

As decisões D3 e D7 favorecem A, com B e C como frentes paralelas curtas.

## 8. Riscos conhecidos

- Ciclo lento: tudo depende de rodadas no console feitas pelo usuário.
- O renderer do nfsmw-nx tem muitas premissas do NFSMW (versão do D3D, endereços,
  passes). Portar sem entender cada uma pode produzir travamentos difíceis de
  diagnosticar sem PC.
- O pack incompleto bloqueia qualquer imagem nativa da cidade e dos menus.
- A memória do processo está no limite (3185/3189 MB no último log). Os caches do
  renderer nativo precisam de limite próprio. O nfsmw-nx usa 512 MB para texturas,
  com descarte.
- O caminho Xenos e o nativo no mesmo NRO aumentam tamanho e manutenção (D8).

## 9. Próximos passos sugeridos para fechar o plano

1. Confirmar ou corrigir as seções 1 a 3 (trava de entendimento).
2. Rodar o NRO de captura pendente e decidir a pergunta 6.1.
3. Escolher entre as abordagens da seção 7 e detalhar o primeiro marco: arquivos a
   portar, o que desligar, critério de aceite e diagnóstico por rodada.
4. Mapear no PPC do Superman os pontos de D3D da pergunta 6.2 antes de portar o código
   que depende deles.
