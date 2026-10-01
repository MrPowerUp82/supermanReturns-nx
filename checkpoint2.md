# Checkpoint de shaders — 2026-10-01

Objetivo: rodar Superman Returns no Sudachi, usando nfsmw-nx como referência.

## Concluído nesta etapa

- Corrigido CUBE no tradutor; oito casos sintéticos passaram com DXC/spirv-val.
- Regenerados 529 shaders; biblioteca com 167 entradas únicas em
  `out/shaders-cube-review/superman_returns_shaders.srsp`.
- NRO compilado com carregador, registro e hook de observação `sub_82383AA8`.
  O corpo PPC confirma descritor tipo 15, ponteiro +48, tamanho +52 e retorno
  de recurso do engine em r3. Não é um construtor D3D confirmado.
- Sudachi carregou as 167 entradas: `out/sudachi/superman_returns_016.log`.
- Teste C++ identificou as 167 entradas e verificou padding, truncamento,
  assinatura, endereço reutilizado e reload. Nove testes Python passaram.
- `tools/project.py package --shader-library CAMINHO` inclui a biblioteca.
- NRO e biblioteca atualizados no SD virtual do Sudachi.
- Ryujinx também confirmou 167 entradas carregadas, mas falhou ao criar o device
  Vulkan (`0x195c` no VA bind, `ErrorOutOfDeviceMemory`).
- Pacote completo gerado em `dist/superman-shader-registry/`.

## Limite atual e próximo trabalho

Os draws ainda usam Xenos. Os SPIR-V offline têm interface diferente de constantes
e texturas; não podem substituir diretamente os módulos Xenos. Falta confirmar
construtores D3D/ligação dos recursos e implementar descriptors, estados de draw,
constantes, texturas e resolve compatíveis. Os candidatos do perfil PC continuam
não confirmados e os endereços de NFSMW não se aplicam a Superman.

Sudachi encerra com `0xC0000005` depois do vínculo de ZCULL, antes de executar PPC.
Isso impede observar o hook durante o jogo e validar imagem. A biblioteca carregou
antes dessa falha. Ryujinx tem problema separado de reserva fixa de GPU VA,
documentado em `docs/validation.md`. Console físico ainda não testado.

Preservar a biblioteca anterior `out/shaders/` e as alterações locais do SDK;
nenhuma dessas etapas comprova uma versão jogável.
