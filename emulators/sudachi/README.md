# Sudachi: build de diagnóstico e correções do emulador

Esta pasta não altera o Sudachi instalado do usuário. Ela documenta o build Linux
usado para reproduzir a falha do port e as correções que pertencem ao emulador.
Detalhes do diagnóstico e da evidência em [docs/vk-probe.md](../../docs/vk-probe.md).

## Origem

O repositório original do Sudachi não está mais público. O código usado é o
espelho [p-yukusai/sudachi-emu](https://github.com/p-yukusai/sudachi-emu),
commit `a7e2127f17841728eaf2ee2eea51aa5a58a85e80` (estado de 2024-09-19). O
espelho não registra os commits dos submódulos; `build-linux.sh` fixa revisões
da era yuzu que compilam com ele (dynarmic `f884bc0`, mbedtls 2.16 `8c88150`,
Vulkan-Headers 1.3.290 etc.). Não é o binário 1.0.15 (`sudachi-refresh-c7431bd`)
do Windows. Ainda assim, o build reproduziu os sintomas registrados no 1.0.15:
o mesmo resultado do probe de plataforma e o processo do emulador encerrado
logo depois da criação do canal com ZCULL, independentemente do backend do host.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `build-linux.sh` | Clona o espelho e as dependências fixadas, aplica os patches e compila `sudachi-cmd`. `stock` = só ajustes de build; `compat` = com as correções. |
| `sudachi-linux-build.patch` | Só compilação com Ubuntu 24.04/GCC 13: FFmpeg do sistema sem `codec_internal.h` (desliga o atalho de decodificação H.264 do NVDEC), `-msse4.1` em `vic.cpp`, avisos de VMA/GLSL não fatais. Sem efeito nos caminhos testados. |
| `sudachi-nvk-compat.patch` | `DmaPusher`: método em subcanal sem `SET_OBJECT` é ignorado com log, em vez de desreferenciar ponteiro nulo. Macro JIT x64: operandos `r0` sempre materializados (`rD = r0 + r0`, `rD = r0 + rB`, `And` com zero) e imediato `2` em `AddImmediate`/`Read`. |

## Pré-requisitos (Ubuntu 24.04)

```sh
apt-get install cmake ninja-build g++ glslang-tools libboost-context-dev libfmt-dev \
  liblz4-dev nlohmann-json3-dev libopus-dev libzstd-dev zlib1g-dev libsdl2-dev \
  libenet-dev libavcodec-dev libavutil-dev libswscale-dev libavfilter-dev libva-dev \
  libdrm-dev libssl-dev libsimpleini-dev libxbyak-dev libvulkan-dev \
  xvfb mesa-vulkan-drivers gdb
emulators/sudachi/build-linux.sh /caminho/de/trabalho compat
```

O build leva cerca de uma hora com 4 núcleos.

## Para o Sudachi instalado (Windows)

Sem recompilar o emulador: usar o NRO com `mesa-switch-superman.patch` e ativar
**Emulation → Configure → Debug → Disable Macro JIT** (grupo avançado da aba
Debug; no INI: `disable_macro_jit=true` e `disable_macro_jit\default=false` em
`[Debugging]`). A correção do JIT só existe para quem compilar o emulador com
`sudachi-nvk-compat.patch`; ela deve ser proposta a um fork ativo do
Sudachi/yuzu, não ao port.
