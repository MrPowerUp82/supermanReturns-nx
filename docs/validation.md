# Verificação local (2026-10-01)

- XEX original identificado pelo SHA-256 suportado antes de qualquer cópia.
- Codegen ReXGlue v0.10.0 concluído: 293 arquivos emitidos, incluindo 145 C++.
  Nova geração sem alterações: 293 arquivos inalterados. Nenhum `REX_FATAL`.
- Manifesto: 311 funções informadas e somente o midasm hook XMA.
  `XmaKickStarvedContext` aparece no código gerado no ponto esperado.
- Manifesto e TOML de configuração analisados; stamp SHA-256 confere com o manifesto.
- Sete testes de preparação/pacote passaram, incluindo edição incorreta,
  ausência de AST, NRO inválido, proteção de destino e conteúdo do pacote.
- Sintaxe C++ do aplicativo, settings, skip-intro, XMA e um arquivo recompilado
  passou com Clang 23.1.2 Windows x64, headers do fork e dependências de headers
  do SDK portátil. Isso verifica interfaces C++, não o ABI libnx/ARM64.
- Configuração e geração CMake do aplicativo com o SDK vendorizado passaram
  no Windows x64/D3D12. Build Ninja gerado; o executável PC não foi compilado.
- Script PowerShell de build analisado sem erros de sintaxe.
- Dependências do SDK exportadas: 18.903 arquivos copiados, cinco patches da
  base preservados. Dependências locais e arquivos originais ficam ignorados.
- Build Switch tentou iniciar e parou na verificação de devkitA64 ausente.
  libnx, nacptool, elf2nro e Mesa NVK também não foram encontrados.
  CMake/Ninja existem no Visual Studio; não estavam no PATH padrão.

Essa primeira rodada terminou sem NRO gerado. A continuação abaixo gerou o NRO;
execução no console, comparação de imagens, desempenho e gameplay continuam sem
validação. O port jogável depende das etapas em [port-status.md](port-status.md).

## Continuação com Docker e Sudachi

O compilador devkitA64 GCC 15.2.0 e libnx estão disponíveis no container oficial
devkitPro. O NRO **de diagnóstico** foi compilado e testado duas vezes no Sudachi:
espaço virtual de 39 bits e inicialização NVDRV/GPU/address-space/channel passaram.
Isso não executa código do Superman. Veja [sudachi.md](sudachi.md).

A sintaxe ARM64 do aplicativo, dos hooks e de um arquivo PPC recompilado passou.
Foi detectado e corrigido o problema dos aliases do gerador portátil com GCC;
uma checagem GCC/ELF confirmou símbolo original forte, alias fraco e substituição
por hook forte. A checagem ARM64 passou com erros de atributos tratados como fatais.
O código gerado permanece descartável e não foi editado.

## NRO do jogo e primeiro boot no Sudachi (2026-10-01, segunda máquina)

- Mesa NVK compilado no Docker (`libvulkan.a`, 95 MB). O checkout do Mesa agora
  é clonado com `core.autocrlf=false`; com CRLF os scripts falhariam no Linux.
- O archive gerado pelo merge MRI do Mesa tinha índice incompleto: os membros
  `core`/`alloc`/`std` do Rust (NAK/NIL) não apareciam e o link acusava
  `core::panicking::panic` indefinido. `build-mesa.sh` agora roda
  `aarch64-none-elf-ranlib` no archive copiado.
- Todos os objetos do SDK e os 145 arquivos recompilados compilaram com GCC 15.2.0
  (`tools/switch/compile-check.sh`); link e empacotamento geraram
  `app/out/switch/superman_returns.nro` (60 MB). `tools/switch/rebuild.sh` faz
  rebuild incremental.
- No Sudachi o NRO inicia: NVK detecta "NVIDIA Tegra X1 (GM20B)", Vulkan 1.3,
  swapchain criado, VFS monta `game_root` (14 entradas).
- Sem hbloader, `envGetOwnProcessHandle()` é inválido. O runtime agora obtém um
  handle real do processo por cópia via IPC local: `MapProcessMemory` não aceita
  o pseudo-handle. O diagnóstico de alias passou no Sudachi e no Ryujinx.
- Limite do emulador: o núcleo de memória mapeia cada chunk nas vistas do 360
  somente no data abort (`RexGmFaultIn`), e páginas protegidas/MMIO do Xenos
  também dependem de faults. O Sudachi apenas registra `Unmapped Write` e não
  entrega o abort ao handler do guest, então a execução trava logo após a
  configuração da memória. Foi acrescentado `switch_eager_memory`, que mapeia
  os chunks antecipadamente em execução direta sem hbloader. O NRO recompilou,
  mas esse caminho completo ainda não foi validado: os testes atuais param
  antes, na inicialização gráfica. MMIO e write watch continuam pendentes.

## Comparação de emuladores e referência NFSMW (2026-10-01)

- Objetivo solicitado: rodar no Sudachi; Ryujinx é um segundo ambiente de teste.
- Comparado o `nfsmw-nx` no commit
  `df2de32ee569873062b8f8d1da8ad0b8d0a90a5d`. As alterações gráficas do patch
  Mesa são equivalentes às já usadas aqui; as diferenças locais são ajustes de
  build. A referência documenta console, mapeamento por faults e necessidade de
  39 bits, sem uma solução documentada para esses emuladores.
- Sudachi 1.0.15: probe tem 39 bits, alias CPU coerente, reserva e mapeamento
  GPU fixos e dinâmicos bem-sucedidos. O jogo atual ainda encerra o emulador com
  `0xC0000005` após inicializar o contexto ZCULL, antes da memória guest.
- Ryujinx Canary 1.3.351: NRO direto tem 36 bits. O padding TLS do aplicativo
  evita a recursão no leitor de nomes de threads e permite registrar a falha
  real do NVK. O profiler de overlays foi desativado nesse tipo de lançamento.
- Falha NVDRV isolada num probe sem código do jogo: reserva GPU fixa de
  `0x400010000` retorna sucesso, mas devolve `0x1000`; reserva de `0x1fffff0000`
  devolve `0x400020000`. Os mapeamentos nos endereços solicitados falham com
  `0x195c`. No Sudachi os endereços devolvidos são iguais aos solicitados e
  os mesmos mapeamentos passam. Reserva dinâmica + mapeamento passa em ambos.
  Isso identifica um problema distinto no caminho de reserva fixa do Ryujinx,
  sem provar ainda um workaround no driver ou boot do Superman.
- `tools/test-ryujinx.ps1` e `tools/test-sudachi.ps1` guardam saída, relatório
  fresco do probe e código de saída em `out/`. Sobreviver ao timeout não é
  evidência de gameplay. O teste gráfico do probe contém somente buffers NVDRV,
  sem shaders, comandos de desenho ou dados do jogo.

## Biblioteca SPIR-V (2026-10-01)

- Pipeline concluído antes deste checkpoint: 529 contêineres válidos, 167 shaders
  únicos; 4 em `baseworl.AST`, 114 em `metropol.AST`, 402 em `objtiv.AST` e
  9 no executável descompactado `../superman_returns_recomp/logs/image.bin`.
  O XEX comprimido não forneceu contêineres diretamente.
- 23 candidatos rejeitados, dos quais 18 no executável sem tabela de constantes.
  A hipótese de serem shaders internos de clear/resolve ainda não foi confirmada.
- `translate.log`: 529 traduzidos, zero pulados. Todos os 529 foram compilados
  com DXC e validados por `spirv-val` na rodada registrada no checkpoint.
  `pack.log` confirma 167 entradas únicas e reencontro dos 529 contêineres.
- Biblioteca local: `out/shaders/superman_returns_shaders.srsp`, 9.081.764 bytes.
  SHA-256 conferido novamente na retomada:
  `a511bbe8c73b3537fc0cf24d380026ad7d2227aeeccbae4f3cc5ae9c0acedfac`.
  Os logs e `out/shaders/provenance.tsv` foram preservados; HLSL, SPIR-V individuais
  e contêineres não estão nessa cópia local do resultado. O pipeline gera esses
  intermediários numa nova execução.
- Na retomada, os sete testes Python de preparação/pacote passaram novamente.
  O NRO foi recompilado durante a investigação de emuladores; não houve nova
  tradução de todos os shaders.

As alterações no tradutor e as versões locais estão em [shaders.md](shaders.md)
e [provenance.json](provenance.json). Na etapa offline a biblioteca ainda não era
carregada pelo aplicativo; a integração abaixo atualiza esse estado. SPIR-V válido
não comprova equivalência visual, execução na GPU ou correção dos hooks.

Nova rodada após revisão de CUBE: `out/shaders-cube-review/` contém 529 HLSL e
529 SPIR-V aprovados, zero falhas DXC, 167 entradas empacotadas e todas
reencontradas. SHA-256 `26d2c05bb357507be0264da585f643e517695ffac66b72383d817db5036ce181`.
Os oito casos sintéticos de `shaders/test_translator.sh` também passaram,
com compilação DXC e validação SPIR-V do HLSL de regressão. Os sete testes
Python de preparação/pacote passaram. Nenhum NRO foi recompilado nesta revisão:
o tradutor offline não faz parte do backend Xenos atual.

## Carregamento no NRO e registro de recursos (2026-10-01)

NRO recompilado com `sr_shader_library.cpp`, `sr_shader_registry.cpp` e
`sr_shader_hooks.cpp`. O teste real no Sudachi confirmou as 167 entradas carregadas
em `out/sudachi/superman_returns_016.log`, linha 2. O emulador voltou a encerrar
com `0xC0000005` depois de vincular ZCULL, antes da execução PPC. Portanto o hook
de recursos tem evidência estática e testes do registro, mas ainda não observação
durante o jogo nem validação visual. Os draws continuam usando Xenos.

O teste C++ do registro passou para todas as 167 entradas e verificou padding,
truncamento, assinatura desconhecida, reutilização de endereço e reload proibido.
Os nove testes Python passaram, incluindo cópia opcional da biblioteca e rejeição
de biblioteca de outro jogo antes de criar o pacote.

Ryujinx também carregou as 167 entradas (`out/ryujinx/superman_returns_003.log`,
linha 2). A GPU falhou em VA bind `0x1fffff0000` com `0x195c`, seguido de
`ErrorOutOfDeviceMemory` e falha de inicialização do aplicativo. O processo foi
encerrado pelo limite do teste; permanecer aberto não comprovou execução do jogo.
Pacote local gerado em `dist/superman-shader-registry/`, com NRO, configuração,
biblioteca e os dados originais do jogo, sem alterar a origem.

## Sessão Linux na nuvem: teste mínimo Vulkan e falha do Sudachi (2026-10-01)

Ambiente: Ubuntu 24.04 x86_64, 4 núcleos, sem GPU. Ferramentas: devkitA64 GCC
15.2.0 e libnx da imagem `devkitpro/devkita64` fixada (extraída para o host),
LLVM/Clang 15.0.7 e SPIRV-Tools v2025.1 do Ubuntu, Rust nightly 1.101.0
(2026-09-30), bindgen-cli 0.73.2, cbindgen 0.29.4, Meson 1.12.1, g++ 13.3.0.

Testes executados:

- `python -m unittest discover -s tests -v`: 15 testes (os 9 anteriores e 6 novos de
  scripts: sem CRLF, `bash -n`, chaves das configurações do probe, SPIR-V embutido,
  patch Mesa separado).
- `shaders/test_translator.sh` com DXC 1.9.2609 Linux (SHA-256 do arquivo igual
  ao de `provenance.json`), g++ 13.3.0 e spirv-val v2025.1: 8 casos CUBE passaram,
  HLSL compilado e SPIR-V validado. Rodou no host, sem a imagem Docker.
- `shaders/test_registry.sh` sem argumento: novo modo sintético, sem dados do jogo
  (24 contêineres fabricados, duplicata colapsada). Carregador rejeitou pacote
  alterado, de outro jogo, truncado, com bytes sobrando e ausente; registro passou
  padding, truncamento, endereço reutilizado e reload. Com a biblioteca real,
  o comportamento anterior continua (caminho como argumento).
- `python tools/fetch_thirdparty.py`: falhava no Linux num link simbólico pendente
  do MoltenVK (no Windows vira arquivo). Agora ignora links pendentes (7).

Docker: o daemon funciona, mas a política de rede desta sessão bloqueia os
espelhos Debian e o Docker Hub passou a limitar pulls. O Mesa foi compilado com o
mesmo `build-mesa.sh` diretamente no host (LLVM 15 do Ubuntu em vez do Debian),
com `/project` e `/work` apontando para o checkout. O fluxo Docker documentado
(`tools/build-docker.sh mesa`) não foi executado até o fim aqui.

Resultado principal: a falha `0xC0000005` do Sudachi foi reproduzida com
`vk-probe.nro` (sem o jogo) num build Linux do Sudachi, explicada e corrigida no
driver; dois problemas do emulador foram isolados. Detalhes e tabela de evidência
em [vk-probe.md](vk-probe.md). O NRO do jogo não foi compilado nem executado nesta
sessão (sem C++ gerado nem arquivos do jogo).

### Ryujinx: reserva fixa (hipótese, não confirmada)

Os valores do probe de plataforma batem com um `AllocSpace` com `FixedOffset` que
devolve o início do bloco livre que contém o pedido, e não o endereço pedido:
`0x400010000` → `0x1000` (primeiro bloco livre começando em `0x1000`) e
`0x1fffff0000` → `0x400020000` (logo após a reserva anterior de 64 KiB). O mapa
no endereço pedido falharia porque a reserva ficou registrada no endereço devolvido.
O código do Ryujinx não está acessível desta sessão para confirmar; não foi feita
nenhuma mudança para o Ryujinx. Próximo passo: comparar com
`NvHostAsGpuDeviceFile.AllocSpace` e, se confirmado, decidir entre corrigir o
emulador ou usar só reservas dinâmicas na camada Horizon do Mesa.
