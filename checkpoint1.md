Sudachi path set. Rebuilding the platform probe NRO to verify the emulator works on this machine.

Sudachi on this machine runs the probe NRO OK (39-bit address space, GPU services open). Driver build and compile check continue in the background; I'll report when they produce errors or finish.

Noise from meson's feature probes (not an error). Mesa is configuring; waiting for further events.

Still meson configure output (second build dir), not an error. Continuing to wait.

The Mesa NVK driver build completed successfully (libvulkan.a, 95MB). The compile check is now working through the 145 recompiled game files after building the SDK runtime without errors — I'll wait for it to finish.

Waiting for the recompiled-code compile to finish.

The first monitor expired as expected; the completion monitor for the compile check is still armed.

No real errors — just filenames containing "error". At 94%, on recompiled file ~107/145. Re-arming the completion watch is still active; waiting.

All objects compiled; only the link failed (expected with the stub driver). Checking whether any undefined symbols are non-Vulkan, then relinking with the real driver.

Link now fails only inside Mesa's Rust compiler (NAK): the Rust `core`/`alloc` std libs aren't bundled in `libvulkan.a`. Checking how upstream packages it.

Diagnosing why the Rust std (core/alloc) inside Mesa's archive doesn't resolve at link time. Using a script file to avoid PowerShell quoting issues.

Link actually succeeded on retry (the first failure looks like a transient read of the 95 MB driver from the OneDrive folder). ELF is 64 MB. Now generating the NRO.

The link line shows CMake split the `--whole-archive` flags away from `-lvulkan`, so the driver is linked as a normal archive. Checking whether the archive's symbol index covers the Rust std members.

The archive index is missing the Rust std symbols. Rebuilding the index with devkitA64's `ranlib`.

`ranlib` fixes the index (the Mesa MRI merge wrote an incomplete one). Adding that to `build-mesa.sh` and applying it to the staged driver.

**NRO built** (`app/out/switch/superman_returns.nro`, 60 MB). Next: run it in Sudachi with the game data. Checking how the app locates its config and game files.

The app falls back to `sdmc:/switch/superman-returns-nx/` when launched directly. Staging the NRO, config, and a junction to the game data in Sudachi's SD folder.

The NRO boots in Sudachi with NVK Vulkan initializing and the game VFS mounting, but it then fails to commit guest memory (0xE401, kernel InvalidHandle). I'll check the start of the log to inspect the memory setup.

The very first commit fails. Looking at the Switch memory-commit code.

Found it: guest memory relies on `envGetOwnProcessHandle()`, which is only valid when hbloader provides it, so launching directly from Sudachi yields an invalid handle (0xE401). I'll add a fallback that fetches a real own-process handle via an IPC self-session so direct launches work correctly.

Making it deadlock-proof: the helper closes the server end after replying, so the waiting request always returns.

Now an incremental rebuild script (syncs only app/SDK sources, keeps mtimes so make rebuilds just what changed).

Rebuilt. Deploying and running in Sudachi again.

The log stops after ~1 s this time — likely a hang or crash right at the new handle code. Checking the emulator log.

The IPC self-session approach hangs under Sudachi with no crash record, so I'll switch to using the `CUR_PROCESS_HANDLE` pseudo-handle for the map SVCs instead, since it works reliably on both Sudachi and real Horizon.

The rebuild with the pseudo-handle fallback shows the log ending earlier with no errors, probably because spdlog only flushes on errors before the emulator gets killed at 60s. I'll check for a flush setting to get a clearer picture of actual progress.

Flush is every 1 s already, so the process really stalls (or is very slow) right around guest-memory setup. Running longer and watching CPU/log growth to tell a hang from slow mapping.

Memory commits succeed now, but Sudachi logs `Unmapped Write` since the guest-memory core relies on lazy page-fault mapping that these emulators silently drop instead of triggering a data abort. I'll check whether the SDK supports eager mapping or has guidance for emulator compatibility.

I've confirmed that mapping into the 360 views relies on fault-driven handling (via `RexGmFaultIn`), but Sudachi silently drops unmapped accesses instead of raising aborts, so gameplay/rendering can't be validated beyond memory setup without real hardware. I'll record these findings in the docs and commit.

O jogo agora compila e gera um NRO para Switch, e ele começa a iniciar no Sudachi. Mas trava logo depois de preparar a memória. Isso é uma limitação do emulador, então a partir daqui os testes de boot, imagem e gameplay precisam ser feitos num console real.

**O que funcionou nesta máquina**
- Refiz as dependências do SDK e o codegen, e compilei o driver Mesa NVK no Docker.
- Todo o SDK e os 145 arquivos recompilados do jogo compilaram para ARM64 sem erros. O resultado é `app/out/switch/superman_returns.nro` (60 MB).
- O NRO de diagnóstico passou no Sudachi em `Music\sudachiemu.org-winpc-1-0-15`. O script de teste já usa esse caminho.
- No Sudachi, o jogo detecta a GPU (NVIDIA Tegra X1, Vulkan 1.3), cria a janela 1080p e monta a pasta do jogo.

**Correções feitas**
- **Link do driver:** o arquivo `libvulkan.a` gerado pelo Mesa tinha um índice de símbolos incompleto, e o link falhava com mais de 4.900 símbolos Rust indefinidos. [build-mesa.sh](tools/switch/build-mesa.sh) agora reconstrói esse índice depois da compilação.
- **Mesa no Windows:** o Mesa agora é baixado sem conversão de quebras de linha (CRLF), que quebraria os scripts dentro do Linux. Também corrigi um erro do PowerShell 5 em [build-docker.ps1](tools/build-docker.ps1).
- **Memória ao abrir o NRO direto:** sem o Homebrew Menu, toda reserva de memória do jogo falhava com o erro `0xE401`. Em [guest_memory_switch.cpp](sdk/src/core/guest_memory_switch.cpp), quando falta o handle que o Homebrew Menu fornece, o código usa outro que o Sudachi aceita. No console continua sendo preciso iniciar pelo Homebrew Menu.
- **Scripts novos:** [compile-check.sh](tools/switch/compile-check.sh) compila tudo sem o driver real, e [rebuild.sh](tools/switch/rebuild.sh) faz o rebuild incremental e copia o NRO.

**Por que trava no Sudachi**
- O código de memória do port só liga a memória do jogo aos endereços do Xbox 360 quando o processador gera uma falha de acesso. Os registradores da GPU do Xbox (Xenos) também dependem dessas falhas.
- O Sudachi apenas registra `Unmapped Write` e ignora o acesso, sem avisar o jogo. Por isso a execução para logo após a configuração da memória.

Registrei tudo em [validation.md](docs/validation.md) e [sudachi.md](docs/sudachi.md). Os 7 testes Python passam.

**Próximos passos**
1. Testar o NRO num console com Atmosphère, iniciando pelo Homebrew Menu. Na pasta `sdmc:/switch/superman-returns-nx/` vão o `superman_returns.nro`, o `superman_returns.toml` e a pasta `game_root` com o jogo.
2. Se quiser, adiciono um modo opcional que liga toda a memória de uma vez, só para o emulador. Isso deve levar o boot no Sudachi mais adiante, mas a GPU continuaria sem receber comandos, então não daria para validar a imagem.

As alterações ainda não foram commitadas; posso commitar se quiser.

---

The installer ships a prebuilt `shaders.nfsp`. Let me see how it's produced in the reference repo.

No, it's not needed for the current build. I'll run two quick checks, then replace the junction with a real copy of the game folder (removing only the link, not the original).

Game copied (13 files, 2 GB; the original is untouched). Now a quick check on whether Superman's data even exposes shader containers the way NFSMW's does:

Não gerei a biblioteca de shaders pré-compilados (`nfsmw_shaders.nfsp`). Na build atual ela não seria usada.

**Cópia do jogo:** feita. Substituí o link pela cópia real de `superman_returns_recomp\game` em `%APPDATA%\sudachi\sdmc\switch\superman-returns-nx\game_root` (13 arquivos, 2 GB). A pasta original continua intacta.

**Por que não gerei a biblioteca de shaders**
- No NFSMW, o `shaders.nfsp` só é lido pelo **renderizador nativo** do próprio app (`nfsmw_shader_hooks.cpp`, `nfsmw_nativo_*`). Esse renderizador intercepta as funções do jogo que criam shaders e escolhe cada shader pelo hash do microcódigo.
- Nosso port ainda usa o backend genérico Xenos do SDK, que traduz os shaders do Xbox 360 para SPIR-V durante o jogo. Esses shaders compilados ficam guardados no cache do Mesa no SD, em `/.mesa/...`.
- Por isso a configuração do port diz que ele "não requer nem aceita `nfsmw_shaders.nfsp`". Gerar a biblioteca agora não teria efeito sem portar antes esse renderizador.
- Os hooks e endereços do renderizador do NFSMW pertencem ao executável dele. Para o Superman, os equivalentes teriam de ser encontrados no XEX dele.

**O que a varredura mostrou:** procurei os cabeçalhos de shader do Xbox 360 nos arquivos do jogo. É uma contagem bruta de assinaturas, ainda não validada, e alguns acertos devem ser falsos positivos.

| Arquivo | Pixel | Vertex |
|---|---|---|
| `objtiv.AST` | 209 | 193 |
| `metropol.AST` | 70 | 44 |
| `baseworl.AST` | 2 | 2 |

- Diferente do NFSMW, o Superman parece usar o formato de cabeçalho mais novo, que é o que o XenosRecomp espera. Se isso se confirmar, o conversor do formato de 2005 não seria necessário.
- O executável não tem nenhum shader solto, e os encontrados em `fmv`, `sound` e `voice` provavelmente são falsos positivos.

**Quando vale a pena:** a biblioteca pré-compilada é principalmente uma otimização. Ela evita travadas na primeira vez que cada shader aparece e permitiu as melhorias de FPS do NFSMW. Antes disso, o boot pelo backend Xenos precisa ser validado num console real, já que o Sudachi não passa da configuração da memória.

Se quiser adiantar, posso começar pela parte que independe do console: adaptar o localizador e o conversor de shaders do NFSMW ao Superman, validar os candidatos, traduzir com o XenosRecomp/DXC e empacotar. Assim a biblioteca fica pronta para quando o renderizador nativo for portado.

---

Vou começar pelo pipeline da referência.

Superman's containers use the newer `0x102A11xx` signature, which XenosRecomp reads natively. Reading the translator, packer and XenosRecomp container layout.

The library format is simple (xxHash of the original container → SPIR-V). Checking what tools my Mesa build image already has (compiler, SPIRV-Tools, DXC).

spirv-val is there; I need DXC for Linux (Microsoft's official GitHub release) and g++ 14 (the `gcc:14` image is local). Checking the DXC release asset.

Downloading `linux_dxc_2026_09_28.x86_x64.tar.gz` (13 MB, official Microsoft DXC v1.9.2609 release) into `.tools/dxc`.

DXC ready. Now setting up `shaders/` with the reusable translator from the reference (XenosRecomp with its fixes, common header, microcode validator) plus new Superman-specific scanner and packer.

Writing a validator for the 2008-style container (same checks the reference applies to its 2005 containers: table bounds, constant names, and control-flow/EXEC validation of the microcode).

520 containers validated (4 baseworl, 114 metropol, 402 objtiv). Checking the 5 rejected candidates and duplicates.

Found 160 unique shaders across 281 pixel and 239 vertex occurrences. Since the XEX is compressed, I'll also check for an already-decompressed image, as a similar port found extra shaders there.

`logs/image.bin` in the recomp project looks like a dumped XEX image. Checking it for shader containers:

The PC recomp project already has its own shader tooling (`tools/shaders/`). Let me look before duplicating work.

Since the PC project's DXIL-based renderer won't work for Vulkan, I'll stick with the NFSMW-style SPIR-V library. Now checking if the ReXGlue host tool can reproducibly dump the decompressed XEX image to get the embedded shaders.

I don't have a reproducible dump in this toolchain, but the decoded `logs/image.bin` (a 10 MB PE file from the PC project) works fine locally. I'll let the scanner pick up extra files since every container gets validated anyway.

Scan complete: 529 valid containers (167 unique), with 9 from the executable; 18 others lack constant tables (likely Direct3D's internal clear/resolve shaders) and can't be translated by XenosRecomp, so they're excluded. Next, I'll build the translator.

Now the library format for Superman (adapted from NFSMW's `nfsmw_shader_library`, accepting 2008 containers, with its own file signature so the two can never be mixed) and the packer.

Now the end-to-end pipeline script (scan → translate → DXC → spirv-val → pack → verify):

All 529 containers translated to HLSL. DXC rejected the first one; looking at why.

The Linux DXC release needs glibc 2.38; the Mesa image is Debian 12. I'll make a small shader-tools image on Debian trixie (glibc 2.41) with g++ and spirv-tools from apt.

Found two NFSMW-specific assumptions causing problems: the pixel constant uniform block is undersized for Superman's register usage (needs up to c229+, Xbox 360 supports 256), and an array-bound macro is underflowing. I'll inspect the generator code next.

Found that Superman's shaders use pixel registers up to c229+, exceeding XenosRecomp's inherited 224-register limit. I'll raise the pixel constant block to 256 and flag it as a Superman-specific change requiring the future renderer to upload 256 pixel constants.

Found that XenosRecomp assumes the CUBE instruction's source is always a temp register (r250), but here it's actually a constant (c250). Looking into how operands encode constants to fix this.

I'll emit the constant's name (raw, like the temp-register path) when `CUBE` reads a constant:

Also masking the temp-register index (bit 7 is the abs flag; the stock path didn't strip it):

443/529 shaders compile; the remaining 86 fail mainly because the translator only declares registers r0–r31 and booleans b1–b3, while Xenos/Superman shaders use higher register counts. I'll locate the declarations to fix this.

Found that the `bN` case only mapped the first element of CTAB bool arrays like `RwSkinUsedWeights`. I'll fix it to map every array element and declare all 128 Xenos temporaries.

Patched the translator (bool arrays, 128 registers). Rerunning the pipeline.

525 of 529 compile. Last issue: a vertex→pixel interpolator with usage `NORMAL0`, which the translator's fixed interpolator list doesn't include. Looking at that list:

Locations follow declaration order on both stages, so appending `NORMAL0` keeps VS and PS consistent:

All 529 shaders now compile with DXC and pass spirv-val. Packing failed due to an NFSMW-inherited check requiring the physical part be a multiple of 12 (only valid for 2005 containers), so I'm relaxing that check for the 2008 layout.

Atingi o limite de uso, então paro aqui. A biblioteca de shaders do Superman foi gerada, mas ainda não é carregada pelo jogo: o renderizador nativo que a usaria ainda não foi portado. Nada foi commitado.

**Resultado**
- O arquivo é `out/shaders/superman_returns_shaders.srsp` (9,1 MB), ao lado dos logs, da `provenance.tsv` (de qual arquivo e posição veio cada shader) e do SHA-256 `a511bbe8…acedfac`.
- Foram encontrados e validados 529 shaders: 520 nos `.AST` do jogo e 9 no executável já descompactado (`logs/image.bin` do projeto recomp). São 167 únicos.
- Todos os 529 foram traduzidos para HLSL, compilados com DXC 1.9.2609 e aprovados no `spirv-val`. Depois de empacotar, cada um foi reencontrado na biblioteca.
- 18 shaders do executável ficaram de fora porque não têm tabela de constantes, e o tradutor (XenosRecomp) não consegue processá-los sem ela. Provavelmente são shaders internos do Direct3D, como clear e resolve.

**Como reproduzir**
- [shaders/build_library.sh](shaders/build_library.sh) faz tudo: localiza, traduz, compila, valida e empacota.
- Roda na imagem de [shaders/Dockerfile](shaders/Dockerfile), baseada em Debian trixie, porque o DXC para Linux exige uma glibc mais nova que a da imagem do Mesa. O DXC fica em `.tools/dxc/`.

**O que foi criado ou adaptado**
- **Formato da biblioteca:** [sr_shader_library.cpp](app/src/sr_shader_library.cpp) segue o formato do NFSMW, mas com assinatura própria (`SRSSPV`), para nunca carregar a biblioteca do NFSMW por engano.
- **Validação e empacotamento:** [sr_container.h](shaders/sr_container.h) valida cada shader, [sr_find_containers.cpp](shaders/sr_find_containers.cpp) varre os arquivos e [sr_pack.cpp](shaders/sr_pack.cpp) empacota.
- **Correções no tradutor**, marcadas como "Superman Returns" no código:
  - 256 constantes de pixel em vez de 224, porque o jogo lê c229 e acima;
  - 128 registradores temporários em vez de 32, porque o jogo usa até r51;
  - todos os elementos de arrays booleanos mapeados (o `RwSkinUsedWeights` do skinning ocupa b0 a b3);
  - a instrução `CUBE` aceita uma constante como origem;
  - o interpolador `NORMAL0` foi adicionado, porque 4 shaders o usam.
- Não apliquei as reescritas que o NFSMW faz no HLSL (sombra PCF, sombra por mínimo, desenfoque), porque dependem dos nomes e da numeração dos shaders daquele jogo.

**Ressalva:** a validação garante que os shaders compilam e geram SPIR-V válido, não que a imagem sai correta. Isso só dá para comparar quando o renderizador nativo estiver rodando.

**O que falta**
1. Atualizar `docs/` (building, validation, provenance) com o pipeline de shaders, as versões do DXC e da imagem Docker e as mudanças no XenosRecomp.
2. Revisar `git status` e commitar, se quiser. Os arquivos gerados, que contêm dados do jogo, já ficam fora do Git, em `out/` e `.tools/`.
3. Portar o renderizador nativo para que ele carregue o `.srsp`: confirmar no executável do Superman as funções que criam shaders, conectar a biblioteca e enviar as 256 constantes de pixel.
4. Testar o NRO num console real, iniciado pelo Homebrew Menu. O Sudachi não passa da configuração da memória.

Quando o limite renovar, é só pedir para continuar a partir do item 1.