# Estado e próximos requisitos

A integração inicial contém o aplicativo ReXApp, os fontes do runtime Switch,
toolchain devkitA64, alvo NRO, caminhos portáveis, controle libnx e o hook XMA
de `0x826595B8`. O hook foi trazido do PC e precisa ser revalidado no Switch:
o fork tem alterações no decodificador XMA. O skip-intro é opcional e desligado.

O backend Xenos traduz os shaders em execução. Não requer nem aceita
`nfsmw_shaders.nfsp`. A biblioteca própria `superman_returns_shaders.srsp` já foi
gerada, validada e carregada pelo aplicativo para identificar recursos; ainda
não substitui os shaders dos draws. Veja
[shaders.md](shaders.md). Não há promessa de 30 FPS ou de imagem correta. As limitações
de funcionalidades Vulkan, memória e desempenho do Tegra X1 precisam ser medidas.

O projeto PC já chega ao título com D3D12. Isso não valida o backend Vulkan ou
a recompilação ARM64. Os endereços candidatos de `native_renderer/game_profile.h`
do PC ainda não estão confirmados; os hooks nativos de NFSMW pertencem a outro jogo.

Etapas para um port jogável:

1. Compilação/link com devkitA64 + Mesa concluídos; NRO disponível localmente.
2. Crash do Sudachi na inicialização NVK: causa encontrada e corrigida no driver
   (`mesa/mesa-switch-superman.patch`), com teste mínimo `vk-probe.nro` passando
   num build Linux do Sudachi, inclusive com o layout de memória eager; no Sudachi
   instalado é preciso "Disable Macro JIT". Ver [vk-probe.md](vk-probe.md).
   Pendente: NRO do jogo com a correção no Sudachi do Windows, depois threads,
   leitura AST e boot. Ryujinx (reserva GPU fixa, 36 bits) continua pendente.
   Estado dos probes em [validation.md](validation.md).
3. Comparar imagens do Vulkan/Xenos com o D3D12 do PC e testar áudio/entrada/saves.
4. Confirmar as funções XDK do Superman com capturas e análise do executável.
5. Adaptar o renderizador nativo Vulkan do NFSMW: estados, tiling/resolve, formatos,
   shaders extraídos dos AST e constantes de cada draw. Manter comparação visual.
6. Perfilar mundo aberto e otimizar CPU/GPU, depois registrar PGO do próprio jogo.

Redução de resolução e remoção de efeitos do projeto PC não foram importadas:
há evidência local de imagem corrompida nessas opções. O port mantém 1280x720.
