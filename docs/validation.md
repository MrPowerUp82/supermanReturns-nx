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

Sem NRO gerado, execução no console, comparação de imagens, desempenho ou
validação de gameplay. A primeira entrega é a estrutura funcional de desenvolvimento,
e o port jogável ainda depende das etapas em [port-status.md](port-status.md).

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
