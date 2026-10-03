# Checkpoint do renderizador nativo — 2026-10-03

Checkpoint solicitado pelo usuário antes do merge na `main` e push.
Estado das fontes: commit `8746c72`, branch `codex/superman-native-gameplay`.
O port nativo está **incompleto**: não há gameplay renderizado ou aceitação de
desempenho no Switch. Este checkpoint preserva uma etapa de implementação.

## O que está implementado

- Perfil Superman, acesso guest validado e cópias físicas canônicas.
- Espelho PM4 e snapshots próprios de estado, argumentos e palavras dos pacotes.
- Corpus de 223 shaders SPIR-V validado e identificação dos objetos de shader.
- Treze hooks D3D, com as chamadas originais preservadas, e captura antes de waits.
- Fundação de associação exata por endereço físico, época e palavras do pacote;
  reconhecimento separado do commit, predicação e proteção contra replay em retry.
- Fila limitada a 512 comandos e 8 MiB de snapshots; operações retiradas continuam
  contabilizadas até sua conclusão. Conclusão não atravessa lacunas de serial.
- Cancelamento de produtores, consumidores e waits; gates explícitos para
  EVENT_WRITE_SHD e WAIT_FOR_IDLE. Não há conclusão simulada por vblank ou draw omitido.

A fila e o ledger estão compilados, mas ainda não ligados ao executor GPU real.
NativeSystem não instala os callbacks de associação enquanto não houver uma
fonte compartilhada e validada de épocas de alocação.

## Evidência real do console

Última versão executada: `d34d897-fix5`, título `SR Native Capture boot5`.
O usuário informou tela preta. O log novo `superman_returns_003.log` confirma:

- Invalidação inicial do vertex cache reconhecida.
- 54 operações capturadas, sem rejeição de captura.
- Próximo bloqueio: EVENT_WRITE_SHD, opcode `0x58`, físico `0x1F4D003C`.
- Zero swaps concluídos e zero frames de gameplay.

SHA-256 do log: `9d4f675c134a67d564cf353dc162fccb2cf6f21c76ffc2b11a13e76c0eea002b`.
Logs antigos de Xenos não contam como evidência do renderizador nativo.

## Artefatos e implantação

O NRO no SD permanece `/switch/superman-returns-nx/superman_returns.nro`.
Configuração e shader pack ficam ao lado dele; não usar subpastas de captura no SD.
FTP informado pelo usuário: `192.168.1.75:5000`.

- NRO fix5 implantado: 60.123.333 bytes, SHA-256
  `6cbb497faa03b40af259c74d2cf4b5ff921f4b4d9b1eb6af0fc383a09d5a561e`.
- Shader pack: 9.806.588 bytes, SHA-256
  `0bfc7650fee6d5b0eb0a3374b2cc3ccaa41aea942ff50b6d3361535370455040`.
- Build local posterior, `2ca2c9e-queue6`: 60.123.333 bytes, SHA-256
  `312591308e351a92dc2703298c8b4116d7c4992e2e1a3b795b20ca7cbeec4679`.
  Compilou para Switch e passou na auditoria dos hooks; **não foi implantado**.

Os artefatos, logs e ferramentas locais continuam fora do Git. O worktree de
continuação é `C:/Users/Gusta/.codex/worktrees/superman-native-gameplay/supermanReturns-nx`.
O ledger detalhado local fica em
`.superpowers/sdd/2026-10-02-renderer-nativo-gameplay/progress.md` nesse worktree.
A referência PC em `C:/Users/Gusta/Documents/outros-projetos/superman_returns_recomp`
foi usada somente para leitura.

## Próximos passos

1. Concluir a Task 5 do plano: registrar épocas no alocador real antes de publicar
   pacotes ou entrar em waits, compartilhando a autoridade entre hooks e CP.
   RingMakeSpace não comprova nova alocação. Auditar o alocador `820FCBE8`, o caminho
   de callback customizado e o fallback fixo usados por LargeSegment `820FCF90`.
2. Validar uma sequência real sem mismatch, replay ou espera circular, mantendo
   a execução GPU desabilitada até comprovar a associação.
3. Concluir a Task 6: ligar queue/ledger e gates à conclusão GPU real do último
   serial reconhecido pelo CP, sem esperar capturas futuras.
4. Implementar as Tasks 7–12: ABI/geometria, recursos, texturas, pipelines,
   resolves e apresentação do frontbuffer real.
5. Executar as Tasks 13–14: validação no console e medição de desempenho.

Na Task 7, corrigir a colisão de locations POSITION1/TEXCOORD7, a location de
NORMAL1 e os offsets/tamanho do bloco compartilhado antes de habilitar shaders.
Não solicitar outro teste do NRO fix5 inalterado: ele ainda produz o bloqueio conhecido.

Plano aprovado: [renderer-nativo-gameplay](../superpowers/plans/2026-10-02-renderer-nativo-gameplay.md).
Estado técnico: [native-renderer](../native-renderer.md).

## Verificação do checkpoint

Executar `tests/test_sr_native.sh` com `SANITIZE=undefined` e a suíte Python
`python3 -m unittest discover -s tests -p 'test_*.py'` no ambiente Docker existente.
Verificar novamente a árvore resultante do merge antes do push. Testes host e
build não comprovam gameplay ou corrigem a tela preta por si só.
