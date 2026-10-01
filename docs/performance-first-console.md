# Primeiro perfil do Switch físico — 2026-10-01

NRO de referência: `superman_returns-bsd-test.nro`, ELF preservado em
`out/console/symbols/superman_returns-bsd-test.elf`. Logs com sampler ativo em
`out/console/stack-sampling-result/`; offsets simbolizados em `symbols.txt`.
O flag `logs/rex/perfil_pilas.flag` foi removido do SD depois de copiar os logs.

## Evidência

Último intervalo: cerca de 0,5 FPS, CPU total 300% nos três núcleos disponíveis.
FGGameRender 100%; FGGameSound 68,7%; FGGameCore 68,3%; Main XThread 53,7%.
Registrados aproximadamente 568.022 yields/s, 26 resolves e 125 draws/frame,
esperas de fences (~798 ms/s) e zero pipelines novos nesse intervalo.

Nos threads de sound/core/main, respectivamente 84,7%, 81,2% e 97,3% das amostras
de PC estão em `svcSleepThread`, pela cadeia `sched_yield` → `MaybeYield` →
`NtYieldExecution_entry`. Os percentuais são de amostras, não de tempo exato.
No render, os PCs se concentram em `sub_820F33E8`, `sub_82468EF8` e
`sub_820FD9A8`. O PPC gerado mostra polling de estado associado à GPU, com delay
`db16cyc` sem implementação, e consulta de campos de thread. Isso não prova a
causa da espera da GPU nem permite tratar a rotina como trabalho descartável.

## Experimento controlado

Adicionar `switch_guest_yield_us` (0..1000, padrão SDK 0). O app de teste usa 50 us
somente em `NtYieldExecution`; mantém resultado e barreira de memória. Não mudar
`MaybeYield` global nem os spinlocks do kernel. `switch_guest_yield_us=0` no TOML
restaura a política anterior. Não alterar resolução, shaders, draws ou resolves
nesse experimento. O BSD test anterior no SD continua como referência binária.

Medir sem sampler invasivo: mesma cena, pelo menos 60 segundos depois do loading,
comparar FPS, CPU dos threads, yields, esperas de GPU e áudio. Uma redução de CPU
sem melhora de FPS é um resultado possível, indicando outros limites. A escolha
de 50 us é um parâmetro experimental, não uma emulação provada do scheduler Xbox.
Se houver regressão de resposta/áudio ou menor FPS, voltar a 0.

NRO ARM64 compilado e enviado como arquivo novo no SD:
`superman_returns-yield-test.nro`, menu "Superman Returns NX Yield test",
59.881.669 bytes, SHA-256
`e8f8179fcbe368f2fa8162f4056535aa31582feb0e9a2f4d36c605899a075602`.
ELF: `out/console/symbols/superman_returns-yield-test.elf`.
O sampler está desativado e o reteste físico desse experimento está pendente.

## Resultado do reteste

Usuário não notou diferença visual. Logs em `out/console/yield-test-result/`
confirmam 50 us configurados, sampler desligado, mesmos ~125 draws / 26 resolves
por frame. Último intervalo: 0,5 FPS, CPU 126%, render ~99,6%, sound 6,3%, core
5,7%, main 4,5%; aproximadamente 324 yields/s e 60.510 sleeps curtos/s.
Esperas de fence ~989,5 ms/s, praticamente iguais ao teste anterior.

Conclusão limitada: backoff reduziu CPU de espera, sem melhorar FPS nesse cenário.
Não explica a limitação gráfica. Padrão do aplicativo restaurado para 0 nos fontes;
o NRO de experimento no SD continua com 50 us, e o BSD test original continua com
0. Não é necessário substituir arquivos para comparar esses dois NRO.

Próximo teste: executar o `vk-probe.nro` já enviado ao SD, guardar `vk-probe.log`
com passos, verificação de pixels e apresentação. Comparar seu tempo com o jogo
antes de propor alterações de renderer/resolução/driver.

## Probe no console físico

Usuário observou cores e fechamento automático. Log copiado via FTP para
`out/console/vk-probe-result/vk-probe.log`: `RESULT PASS (0 failed steps)`,
120/120 frames apresentados e teardown concluído. Device, submit vazio, copy,
fill, clear, draw, depth_draw e WSI passaram com verificação dos pixels/buffers.
O probe foi executado sem configuração extra e com hbloader/39 bits.

O fechamento foi normal. Isso valida operações Vulkan básicas nessa compilação,
não o custo ou a equivalência visual do renderer Xenos. O log atual do probe
não contém durações por etapa; não inferir FPS ou latência a partir de PASS.
Próxima investigação deve medir o workload real (draws, resolves, texturas,
sincronização e seus tempos) e identificar a cena/defeito visível antes de
alterar shader, resolução ou efeitos.

Imagem parcialmente corrompida segue sem causa confirmada. Placeholders de
pipeline aparecem durante compilação inicial, mas não explicam necessariamente
defeito persistente. Necessária descrição/captura da cena e comparação controlada.
