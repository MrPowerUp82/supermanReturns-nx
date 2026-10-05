# Superman Returns NX

Port experimental da versão Xbox 360 de **Superman Returns** para Nintendo Switch
(homebrew/libnx), baseado no runtime Switch de
[StevensND/nfsmw-nx](https://github.com/StevensND/nfsmw-nx) e no projeto local
`../superman_returns_recomp`.

**Estado: NRO compilado; inicialização parcial no Sudachi, sem teste no console.
Não é uma versão jogável validada.** O padrão (`sr_renderer = "xenos"`) é o backend Xenos sobre
Mesa NVK. Com `sr_renderer = "native"` o jogo é desenhado pelo **renderer Vulkan do projeto PC**
(`app/src/pcvk/`, ver [docs/pcvk-import.md](docs/pcvk-import.md) e
[docs/native-renderer.md](docs/native-renderer.md)): hooks D3D → captura → Vulkan, com shaders
traduzidos offline (`tools/vkshaders/build_pack.py`). Esse caminho foi validado em GPU de host
(lavapipe) e com um guest sintético; **ainda não foi compilado em NRO nem visto no console**.
As otimizações específicas de NFSMW não são usadas.

- `sdk/`: ReXGlue com memória, threads, áudio, entrada libnx e Vulkan do NFSMW-NX.
- `app/`: aplicativo Superman, manifesto de recompilação e correção de espera XMA.
- `tools/`: preparação com validação de SHA-256, codegen, build e pacote local.
- `mesa/`: instruções e patch NVK da base; precisa ser compilado separadamente.
- `config/`: configuração conservadora, resolução original e vídeos habilitados.
- `shaders/`: geração local da biblioteca SPIR-V `.srsp`, carregada no NRO para
  identificar recursos; os draws do backend Xenos ainda usam Xenos. Veja [pipeline de shaders](docs/shaders.md).
- `app/src/pcvk/` e `tools/vkshaders/`: renderer nativo Vulkan importado do projeto PC e o gerador do
  pack de shaders `.srvk` que ele usa no console. Testes de host: `tests/test_pcvk.sh`.

O manifesto suporta Title ID `454107ED`, Media ID `64A4002A`, versão `0.0.0.1`,
SHA-256 `c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b`.
Os arquivos originais, C++ recompilado e binários ficam fora do Git.

## Preparação

Python 3.10+ e uma cópia própria extraída do jogo são necessários. A preparação
usa por padrão `../superman_returns_recomp/game`; copia somente o XEX, mantendo
os 12 AST no projeto original até criar o pacote.

```powershell
python tools/project.py prepare
python tools/project.py codegen --rexglue ../superman_returns_recomp/.tools/rexglue-sdk/win-amd64/bin/rexglue.exe
python tools/fetch_thirdparty.py
python tools/project.py check --mesa-sdk C:/mesa-sdk/opt/devkitpro/portlibs/switch
powershell -File tools/build.ps1 -MesaSdk C:/mesa-sdk/opt/devkitpro/portlibs/switch
python tools/project.py package
```

O recompiler portátil v0.10.0 permite gerar o primeiro código; para experimentar
as alterações de codegen da base, compile o host em `sdk/` e passe esse executável.
Nenhuma opção de registradores ou perfil PGO de NFSMW foi aplicada ao Superman.

O pacote será criado em `dist/superman-returns-nx/`. Copie para
`sdmc:/switch/superman-returns-nx/` e inicie em title takeover, segurando R ao abrir
um título instalado. O runtime exige memória de aplicação e espaço virtual de
39 bits; o modo Álbum não atende esse requisito.

Veja [build detalhado](docs/building.md), [limitações e migração gráfica](docs/port-status.md)
e [origem dos fontes](docs/provenance.json). Testes locais das ferramentas:
`python -m unittest discover -s tests -v`.

Código novo e integração sob GPL-3.0; SDK e Mesa mantêm suas licenças próprias.
Consulte [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
