# Diagnóstico no Switch físico após o PR #1

O teste informado pelo usuário ocorreu no hbmenu em modo full. A foto mostra
"O software foi fechado porque ocorreu um erro", sem código ou endereço.
Essa tela não distingue falha de GPU, exceção CPU, inicialização ou encerramento.
Não atribuir a falha ao modo applet ou ao problema do Sudachi sem os logs.

## Evidência coletada por FTP em 2026-10-01

Logs preservados localmente em `out/console/2026-10-01-before-update/`, incluindo
NRO do SD, `rex_crash.log`, stderr, log do jogo e relatório Atmosphère.
NRO do SD: 59.852.997 bytes, SHA-256
`380a02e62fbe47b5a6f85ea1b10e5143e6071ebd916643102bab5eb12508373c`.
Ele não registra carregamento da biblioteca e difere do NRO reconstruído da main.

A GPU inicializou e houve execução PPC/carregamento de AST. O erro foi leitura
guest `0x18`, instrução ARM64 `0xb8644a82`, PC relativo `0x1eece54`.
O relatório Atmosphère registra `2345-0102`, disparado pelo handler do SDK após
a exceção. Os offsets do NRO antigo não foram aplicados diretamente ao ELF novo:
uma sequência de 24 bytes em torno da instrução aparece uma única vez no NRO
recompilado, em `0x1ef09d4`. O ELF correspondente identifica
`__imp__sub_82567680`, leitura PPC `lwz r3,24(r31)` com objeto nulo.

O chamador `sub_825674B8` cria um socket por `sub_8246F470` → `NetDll_socket` e
retorna zero se receber -1. A rotina posterior não protege o objeto nulo.
`XSocket::Initialize` chama o socket POSIX; não havia `socketInitialize*` no app
ou SDK. `NetDll_WSAStartup` no caminho POSIX apenas preenche WSADATA. Assim,
inicialização BSD ausente é uma causa provável, ainda sem errno do teste antigo.

Correção em teste: inicializar BSD em `OnPostInitLogging`, antes dos threads PPC,
e registrar errno se criar socket falhar. Não alterar a leitura original nem
simular sucesso de rede. O serviço vive até o encerramento do processo. Reteste
deve mostrar `Switch BSD sockets initialized`, ausência da mesma leitura nula
e avanço do boot; caso contrário o errno e o novo crash definirão a próxima etapa.

Mesa e NRO recompilados após o merge; os 15 testes Python passaram no container
Linux e o teste sintético de registro também passou. No Windows, `bash` aponta
para um WSL sem `/bin/bash`, então o teste de sintaxe de scripts foi executado
no container em vez desse launcher.

Enviados ao SD por FTP, como arquivos novos no diretório do port:

- `superman_returns-bsd-test.nro`: 59.877.573 bytes, menu
  "Superman Returns NX BSD test", SHA-256
  `d55a812cef13da4d7345d752a2058cb85ebc0258d1ec6c0b3f448799b0538f6e`.
- `vk-probe.nro`: 15.022.277 bytes, driver Mesa atualizado.

O arquivo `superman_returns.nro` do SD não foi substituído. ELF do teste BSD:
`out/console/symbols/superman_returns-bsd-test.elf`. Reteste físico pendente.

O PR #1 (`7fce0a4`, merge) corrigiu problemas verificados num probe Vulkan no
Sudachi Linux. O jogo e o console físico não foram testados nessa sessão cloud.
Merge de fontes não atualiza um NRO já copiado para o cartão: Mesa está linkado
estaticamente e precisa ser recompilado, seguido do link do NRO.

## Antes de substituir o NRO

Copie a pasta `sd:/switch/superman-returns-nx/logs/` inteira para o PC. Ela pode
conter logs do jogo e `rex/rex_stderr.log` e `rex/rex_crash.log`. Os arquivos de
stderr/crash podem ser reescritos numa nova execução. Se o port estiver em outra
pasta, procure `logs/` ao lado do NRO. Ausência de crash log não comprova ausência
de falha: o processo pode terminar antes de gravá-lo.

Guarde também qual NRO foi usado (arquivo ou SHA-256), configuração e como foi
aberto. Copie eventual relatório do sistema/Atmosphère correspondente ao horário
do teste, se disponível. Não associe um relatório antigo ao teste atual.

## Sequência de teste

1. Usar o NRO reconstruído com Mesa atualizado e preservar os dados do jogo.
2. Abrir `vk-probe.nro` no mesmo hbmenu full. Ele não precisa de arquivos do jogo.
   O probe usa os caminhos fixos `sdmc:/switch/superman-returns-nx/vk-probe.cfg`
   e `vk-probe.log`, mesmo quando seu próprio NRO é lançado de outro diretório.
3. Guardar `vk-probe.log`. `RESULT PASS` e as verificações de pixels/apresentação
   validam esse teste mínimo; não validam o renderer do jogo.
4. Abrir `superman_returns.nro` e guardar novamente os logs. Comparar a última
   etapa concluída com o probe e o ponto de falha do aplicativo.
5. Usar o ELF correspondente àquele NRO para interpretar endereços do crash.
   ELF de outra compilação pode fornecer símbolos incorretos.

Se o probe falhar, começar pelo driver/serviços/GPU. Se passar e o jogo falhar,
usar os logs para separar memória PPC, MMIO, threads, I/O, shaders e teardown.
Não alterar vários caminhos ao mesmo tempo: cada hipótese precisa de um teste
e da comparação com a compilação anterior.
