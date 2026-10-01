# Checkpoint 4 — console físico e continuação dos shaders pré-compilados

## Prioridade atual: Claude Desktop local

**Direção explícita do usuário:** focar nos shaders pré-compilados. A imagem que
apareceu no console era o logo da EA Games, chuviscado, acompanhado de som e
muita lentidão. Não houve confirmação de menu, cidade ou gameplay. A causa do
chuvisco ainda não foi estabelecida; não presumir que corrigir shader resolve
vídeo, upload de textura, formato/endian, resolve ou sincronização.

Continuar neste checkout local:
`C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx`.
As correções BSD, experimento de yield e documentos desta sessão ainda são
alterações locais sobre `7fce0a4`. Conferir `git status` e preservar o trabalho;
não depender só de um clone do GitHub. A próxima implementação deve ligar os
SPIR-V pré-compilados aos draws, em vez de apenas gerar/copiar a biblioteca.

### O que já existe e deve ser reaproveitado

- Biblioteca real em `out/shaders-cube-review/superman_returns_shaders.srsp`:
  529 contêineres, 167 shaders únicos, 9.081.800 bytes; SHA-256
  `26d2c05bb357507be0264da585f643e517695ffac66b72383d817db5036ce181`.
  Todos compilaram com DXC e passaram no spirv-val. Intermediários HLSL/SPIR-V e
  contêineres estão nessa pasta. Biblioteca anterior em `out/shaders/` preservada.
- Carregador/registro em `app/src/sr_shader_library.*`, `sr_shader_registry.*` e
  `sr_shader_hooks.cpp`. No console testado a biblioteca estava ausente no SD;
  fallback Xenos manteve a execução. Mesmo presente, atualmente ela só identifica
  recursos, **não é usada para renderizar os draws**.
- Hook `sub_82383AA8`: descritor tipo 15 em r4, ponteiro/tamanho +48/+52, retorno
  de recurso do engine em r3. Evidência estática no PPC gerado. Recurso do engine
  não é objeto D3D/pipeline Vulkan. Observação durante execução ainda pendente.
- Correções do tradutor, oito testes CUBE e testes do carregador/registro em
  `shaders/`. O helper CUBE não cobre toda semântica aritmética possível.
- Projeto PC em `C:/Users/webpa/OneDrive/Documentos/projetos/superman_returns_recomp`,
  com `port/src/native_renderer/`, arquivos originais em `game/` e dump
  `logs/image.bin`. Código PPC disponível em `app/generated/default/` deste NX.
- Referência principal: https://github.com/StevensND/nfsmw-nx ; checkout de
  referência local em `.tools/nfsmw-reference/`. Revisões e ferramentas estão em
  `docs/provenance.json`. Backend atual em `sdk/src/graphics/vulkan/`.

### Trabalho prioritário para os pré-shaders

1. Ler `docs/shaders.md` e estudar o renderer nativo da referência. Confirmar no
   executável do Superman criação, bind e destruição de objetos D3D e a ligação
   com os recursos já identificados. Registrar ABI e evidência de cada endereço.
   Os candidatos `826A8050/827A6130` do perfil PC continuam não confirmados;
   endereços de NFSMW não pertencem a Superman.
2. Capturar/identificar o contêiner completo antes de patches de microcódigo;
   garantir associação por objeto, estágio e ciclo de vida. Usar hash com
   comparação integral e invalidar endereço reutilizado. Medir shaders conhecidos,
   desconhecidos e binds durante o boot, sem fingir que o hook já foi validado.
3. Levantar a interface real dos SPIR-V e implementar a ligação Vulkan correta:
   descriptors, constantes VS/PS (256 float4), boolean/int/loops, vertex input,
   interpoladores, samplers/texturas, estados de pipeline, render targets e
   sincronização. **Não inserir diretamente os SPIR-V offline nos pipelines Xenos:**
   o ABI de constantes e texturas é diferente.
4. Implementar a menor rota de draw verificável, com opção de comparação/fallback
   Xenos; ampliar formatos/endian/tiling, cubemap/mips, buffers, resolve e estados
   conforme os draws observados. Shaders sem CTAB e operações desconhecidas
   precisam de comportamento explícito. Não desligar efeitos para esconder defeitos.
5. Validar incrementalmente: carregar pack, identificar shader, criar módulo e
   pipeline, executar draw com dados corretos, comparar captura e medir tempos.
   Registrar separadamente tradução válida, shader usado na GPU e imagem correta.
   Usar o logo EA chuviscado como primeiro cenário de comparação; confirmar se
   aquele draw realmente usa shaders cobertos pelo pack ou uma rota de vídeo.

Não dedicar a próxima etapa a novos ajustes de yield ou overclock. O usuário
priorizou pré-shaders; perfil e logs servem para validar o efeito dessa integração.
Se faltarem partes do renderer para ligar um shader, implementá-las com escopo e
limitações explícitos. Não prometer FPS só porque o shader passou no spirv-val.

### Validação e arquivos de teste

O teste físico BSD já passou do crash anterior: som e logo EA, sem gameplay
validado. Probe Vulkan no mesmo console: PASS, 120/120 frames, cópia, pixels,
desenho e profundidade conferidos. Isso valida Vulkan básico, não o renderer Xenos.
Desempenho atual ~0,5 FPS. Yield de 50 us reduziu CPU ~300% → 126% sem ganho de FPS;
fontes restaurados para padrão 0. Sampler invasivo foi removido do SD.

Logs preservados em `out/console/bsd-test-result/`, `stack-sampling-result/`,
`yield-test-result/`, `vk-probe-result/` e `2026-10-01-before-update/`.
ELFs correspondentes em `out/console/symbols/` (não misturar offsets entre builds).
SD via FTP informado pelo usuário: `192.168.100.37:5000`, diretório do port
`/switch/superman-returns-nx/`. NRO de referência no SD:
`superman_returns-bsd-test.nro`; variante de yield separada. Preservar os arquivos
originais e recolher os logs antes de retestes que possam sobrescrevê-los.

Build incremental após preparar Mesa/codegen/dependências:

```powershell
docker run --rm --mount "type=bind,source=C:/Users/webpa/OneDrive/Documentos/projetos/supermanReturns-nx,target=/project" --mount type=volume,source=superman-returns-nx-check,target=/work -e JOBS=4 devkitpro/devkita64:latest bash /project/tools/switch/rebuild.sh
```

Esse volume já foi usado localmente; `rebuild.sh` não é build limpo. Instruções
completas em `docs/building.md`. Não publicar dados do jogo, shader pack privado
ou fontes gerados. Entregar alteração, evidência, comandos/testes executados e
limitações restantes para revisão e merge.

## Histórico desta sessão

Main atualizada para o merge do PR #1, commit `7fce0a4`. O usuário confirmou
lançamento full, não applet. Os logs e NRO foram copiados do SD via FTP para
`out/console/2026-10-01-before-update/` antes de qualquer reteste.

O console passou pela inicialização Vulkan e executou PPC/leitura AST. O crash
foi leitura guest `0x18`, objeto nulo em `sub_82567680`. Identificação verificada
por sequência ARM64 única no NRO reconstruído e símbolos do ELF. A criação desse
objeto usa `NetDll_socket`; falha retorna nulo. Faltava inicialização BSD libnx.

Correção local: `socketInitializeDefault()` após log e antes de guest threads,
mais errno quando criar socket falha em `sdk/src/system/xsocket.cpp`.
Não se alterou o PPC gerado nem se fingiu sucesso de rede. Hipótese precisa do
reteste para confirmar que resolve o boot. Detalhes em `docs/console-diagnosis.md`.

Driver Mesa atualizado e NRO reconstruídos. Testes: 15 Python no container Linux,
registro sintético C++ e link ARM64 bem-sucedidos. Novos NRO enviados ao SD:
`superman_returns-bsd-test.nro` (menu "Superman Returns NX BSD test") e
`vk-probe.nro`. Original preservado. Logs novos continuam ao lado do NRO.
ELF correspondente guardado em `out/console/symbols/`.

Próximo passo: usuário abrir BSD test no hbmenu full, informar comportamento e
voltar ao FTP para coletar logs. Verificar mensagem de BSD e avanço além da falha;
se socket falhar, usar errno para definir próxima correção. Sem promessa de título
ou gameplay. Alterações desta correção ainda locais, não publicadas em PR.

## Reteste físico: áudio e imagem (2026-10-01)

Usuário confirmou abertura com som e imagem parcialmente bugada, desempenho muito
baixo. Logs preservados em `out/console/bsd-test-result/`. A inicialização BSD
passou e o boot avançou além do acesso nulo anterior. O crash log ali ainda é o
antigo; não há nova exceção registrada no log `superman_returns_002.log`.

Perfil: aproximadamente 0,5 FPS nos intervalos finais, CPU total ~299% em três
núcleos disponíveis; FGGameRender ~99,6%, FGGameSound ~69%, FGGameCore ~67,8%,
Main XThread ~51,6%. Há espera de GPU, yields frequentes e ~125 draws / 26 resolves
por frame. Criação de pipelines caiu para zero nos intervalos finais, portanto
compilação inicial não basta para explicar lentidão sustentada. Amostragem de
pilhas estava desligada. Não há prova ainda da função que domina o tempo.

Preparado `logs/rex/perfil_pilas.flag` no SD via FTP para coleta temporária de
pilhas no próximo lançamento. Usuário deve executar o mesmo BSD test por cerca
de 60 segundos, voltar ao FTP e avisar. Coletar `rex_perfil.log` e interpretar
os offsets com `out/console/symbols/superman_returns-bsd-test.elf`; remover o flag
depois da coleta. Não medir FPS final com sampler invasivo habilitado.

Imagem identificada pelo usuário: logo EA Games chuviscado. Há aviso de frame ignorado por
pipeline placeholder durante compilação assíncrona, mas isso não comprova a
causa de corrupção persistente. Biblioteca .srsp ausente no SD; atualmente só
habilitaria identificação, sem mudar os draws Xenos, então copiá-la não resolve
automaticamente imagem ou FPS.

## Coleta de pilhas e experimento de yields

Pilhas copiadas em `out/console/stack-sampling-result/`, interpretadas com ELF
BSD test; flag invasivo removido do SD. Sound/core/main ficam majoritariamente
em `NtYieldExecution` → `sched_yield` → `svcSleepThread`, ~568 mil yields/s.
Render faz polling em `sub_820F33E8`, `sub_82468EF8`, `sub_820FD9A8`; não foi
alterado. Detalhes em `docs/performance-first-console.md`.

Experimento local `switch_guest_yield_us`: SDK padrão 0, app de teste 50 us só
no export NtYieldExecution, preservando resultado e barreira de memória.
NRO "Superman Returns NX Yield test" enviado ao SD com nome separado, ELF
preservado. Reteste sem sampler: cerca de 60 s na mesma cena, voltar ao FTP.
Não concluir melhora até comparar perfil/FPS e comportamento de áudio.

Reteste Yield: CPU caiu ~300% → 126%, FPS ficou em 0,5 e waits de GPU praticamente
iguais. Padrão nos fontes restaurado para 0. Probe físico executado em seguida:
`RESULT PASS`, 120/120 frames, zero falhas, pixels/copy/depth conferidos. Cores e
fechamento eram o comportamento normal. Log em `out/console/vk-probe-result/`.
Isso valida Vulkan básico, sem validar desempenho/semântica do Xenos. A direção
mais recente do usuário é priorizar pré-shaders, conforme o briefing no topo.
