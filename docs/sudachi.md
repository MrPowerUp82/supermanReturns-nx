# Testes com Sudachi

Emulador indicado: `C:/Users/Gusta/Downloads/sudachiemu.org-winpc-1-0-15`.
O `sudachi-cmd.exe` aceita `--game`, `--config` e `--program`, permitindo testes
por linha de comando. Não foi preciso fornecer novos arquivos ao emulador.

O primeiro teste usa **platform-probe.nro**, um diagnóstico libnx sem código do jogo:

```powershell
docker run --rm --mount type=bind,source=C:/Users/Gusta/Documents/outros-projetos/supermanReturns-nx,target=/project devkitpro/devkita64:latest bash /project/tools/switch/build-probe.sh
powershell -File tools/test-sudachi.ps1
```

Resultado em 2026-10-01: NRO ARM64 compilado com devkitA64 GCC 15.2.0, carregado
no Sudachi, espaço virtual de 39 bits confirmado e chamadas `nvInitialize`,
`nvGpuInit`, `nvAddressSpaceCreate` e `nvGpuChannelCreate` retornando sucesso.
O relatório ficou em `out/sudachi/platform-probe.log`. O log do emulador identifica
o build `sudachi-refresh-c7431bd` e registra ioctls NVDRV parcialmente implementados.

Esse teste verifica o carregamento homebrew e parte dos serviços exigidos pelo port.
Ainda não verifica execução PPC recompilada, XMA, shaders NVK, imagem ou gameplay.
O diagnóstico não implica que todos os comandos usados pelo Mesa funcionem no Sudachi.

O script limita a sessão a 45 segundos, salva os logs e encerra apenas o processo
que iniciou. O SDL usa o SD virtual padrão em `%APPDATA%/sudachi/sdmc`; o relatório
fica em `switch/superman-returns-nx/`. O emulador permanece com seus dados existentes.
