# Compilação

Requisitos: devkitPro/devkitA64, libnx e ferramentas nacptool/elf2nro, CMake 3.25+,
Ninja, Python 3.10+ e Mesa Horizon NVK estático. O SDK precisa das bibliotecas
exportadas por `python tools/fetch_thirdparty.py`; o script preserva os arquivos
modificados pela base. As marcas `.rex-dependency-complete` permitem verificar
exports sem metadados Git. A cópia parcial de `sdk/thirdparty/` não basta.

## Codegen

Execute `python tools/project.py prepare --source <pasta-do-recomp>` e
`python tools/project.py codegen --rexglue <executavel-host>`.
O SHA-256 é obrigatório: outros XEX exigem análise e endereços próprios.
O código é gerado em `app/generated/default/`, ignorado pelo Git. O build usa
`app/cmake/rexglue.cmake`, porque o codegen sobrescreve `generated/rexglue.cmake`.
Após editar o manifesto ou o XEX, execute codegen novamente antes do build.

Para compilar o host do fork, num terminal com Clang e dependências disponíveis:

```sh
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/host --target rexglue --parallel 4
```

No Windows use o terminal de desenvolvimento do Visual Studio com Clang 20+.
O host e o Switch têm builds separados; não execute o recompiler ARM64 no PC.

## Mesa e Switch

Há também um fluxo Docker para usar devkitA64/libnx sem instalá-los no Windows.
Inicie o Docker Desktop e execute:

```powershell
powershell -File tools/build-docker.ps1 -Jobs 4
```

O script usa a imagem oficial devkitPro fixada por digest, obtém a revisão Mesa
`1a8c1a66d6fd8d65f10107c4627ffc3606ba5631`, aplica o patch e cria uma imagem com
LLVM, SPIRV-Tools e Rust. O primeiro build leva mais tempo. O driver fica em
`.tools/mesa-sdk/`; o cache de compilação fica no volume Docker
`superman-returns-nx-build`. `-DriverOnly` prepara somente o driver.
O script preserva checkouts existentes com outras modificações.

O aplicativo aplica `sr_recomp_compat.h` durante a compilação GCC do código
recompilado: o gerador portátil v0.10.0 põe atributos de alias numa posição que
o GCC ignora. A integração usa a definição corrigida da base Switch, sem editar
os fontes gerados. `tools/switch/check-alias.sh` verifica que um hook forte
substitui o alias fraco, mantendo o símbolo da implementação original.

O NRO de diagnóstico e os testes do emulador estão em [sudachi.md](sudachi.md).

Siga [mesa/README.md](../mesa/README.md). Essa pasta mantém o patch da base para
mesa-switch `1a8c1a66d6f` (`mesa-switch-nfsmw.patch`) e, aplicado depois dele,
`mesa-switch-superman.patch`, a correção de compatibilidade com o Sudachi descrita
em [vk-probe.md](vk-probe.md). Os scripts aplicam os dois, nessa ordem.

### Linux/macOS (sem PowerShell)

`tools/build-docker.sh` faz o mesmo que `build-docker.ps1`, sem depender de nenhum
arquivo preparado no Windows: clona o Mesa no commit fixado, aplica os patches,
gera `.tools/mesa-build-source.tar` e roda os containers.

```sh
tools/build-docker.sh mesa       # driver em .tools/mesa-sdk/
tools/build-docker.sh vk-probe   # out/probe/vk-probe.nro, teste mínimo sem o jogo
tools/build-docker.sh game       # NRO do jogo (exige codegen e exports do SDK)
```

Atrás de um proxy HTTPS que intercepta TLS (CI, sandboxes), defina
`BUILD_PROXY_CA=<bundle CA>` e `HTTPS_PROXY`: o script cria uma imagem base
local que confia na CA e usa espelhos apt HTTPS. Sem essas variáveis, a imagem
fixada é usada sem mudança.

`build-mesa.sh` agora reextrai o tarball do Mesa a cada execução (o tar preserva
os horários; o ninja só recompila o que mudou). Antes, um volume já preparado
mantinha a fonte antiga e uma mudança de patch não chegava ao driver.
`MESA_NATIVE_SETUP_ARGS` permite fixar o LLVM 15 em hosts com várias versões
(`--native-file` com `llvm-config = '/usr/lib/llvm-15/bin/llvm-config'`).

`rebuild.sh` é só incremental: exige `/work/game-check` e `/work/superman-source`
criados por `compile-check.sh` e falha com mensagem clara sem eles.
`compile-check.sh` usa um `libvulkan.a` vazio e tolera a falha de link: não é
evidência de NRO compilado.
`-MesaSdk` aponta para `opt/devkitpro/portlibs/switch` contendo `lib/libvulkan.a`.

```powershell
powershell -File tools/build.ps1 -MesaSdk C:/mesa-sdk/opt/devkitpro/portlibs/switch -DevkitPro C:/devkitPro -Jobs 4
```

Em Linux, após instalar as mesmas dependências:

```sh
cmake -S app -B app/out/switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/tools/switch/cmake/switch-devkitA64.cmake" \
  -DREXGLUE_SWITCH_NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch
cmake --build app/out/switch --parallel 4
```

LTO vem desligado para facilitar o primeiro boot; `-Lto` ou `-DSR_LTO=ON` habilita.
Não usar `pgo/` nem `orden_funciones.ld` de NFSMW: descrevem outro executável.

## Pacote e teste

`python tools/project.py package` verifica o magic NRO e o XEX, copia todos os
arquivos da pasta original e cria a configuração. Recusa sobrescrever um pacote
existente; use `--output` para outro destino. A compilação não comprova boot.
Após iniciar no console, recolha `rex*.log`, teste abertura, áudio, tela de título,
Start, save/load e gameplay. Registre firmware, modelo e modo portátil/dock.

## Biblioteca de shaders (identificação opcional no NRO)

O pipeline em `shaders/build_library.sh` extrai os contêineres do próprio jogo,
traduz para HLSL, compila para SPIR-V, valida e empacota em
`superman_returns_shaders.srsp`. É um build host separado de Mesa e do NRO.
O backend Xenos atual continua traduzindo shaders em execução: gerar ou copiar
o `.srsp` habilita a identificação de recursos, mantendo os draws no Xenos.
Para incluí-lo no pacote local:

```powershell
python tools/project.py package --shader-library out/shaders-cube-review/superman_returns_shaders.srsp --output dist/superman-shader-registry
```

A biblioteca deve ficar ao lado de `superman_returns.nro`. Arquivo ausente ou
inválido gera um aviso no log. O comando Docker, as versões verificadas
e os requisitos do futuro renderizador estão em [shaders.md](shaders.md).
