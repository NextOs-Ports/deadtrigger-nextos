# Infraestrutura segura de testes

[`test-matrix-v1.json`](test-matrix-v1.json) classifica cada gate em quatro grupos:

- `pure`: somente leitura/validação estática;
- `filesystem`: escreve apenas numa árvore `mktemp` própria e não envia sinais;
- `process`: testa supervisão/sinais exclusivamente no user/PID/mount namespace
  selado;
- `hardware`: nunca executa automaticamente no host.

Todo gate automático passa por `framework/tools/run-logged.sh`. A ordem canônica é
`test_infrastructure.py` primeiro; se a barreira falhar, nenhuma suíte maior é
autorizada. Teste físico continua separado e exige autorização de IP da sessão atual.

O M07 acrescenta um ledger estático de 20 requisitos e um gate de release
NXExtract completo (`--require-ui`): 56 casos sintéticos, runtime isolado,
pinagem integral, membros regulares/não vazios e auditoria de todos os ELFs.

O ARMv7 tem dois gates separados. `nxloader-armv7-cross` compila com GCC e
Clang/LLD no mesmo sysroot antigo, audita ABI/PT_INTERP/glibc e executa apenas
probes host cross-safe em QEMU; ele declara explicitamente zero ELF/initializer
guest. `nxloader-armv7-physical` é manual e contém somente o probe de cache
ARM/Thumb, sem endereço ou comando remoto embutido.

O ledger `nxloader-m09-audit` fecha os 20 requisitos ARMv7, confere as 56 linhas
ABI de KOTOR/TASM2, preserva a separação entre cross e hardware e valida a prova
física sanitizada. O endereço, hostname, credencial e comando remoto nunca entram
no repositório; o gate automático apenas lê o registro já produzido na sessão
autorizada.

O auditor também compara o runner com a matriz: um comando automático não pode aparecer,
sumir ou mudar seus argumentos sem atualizar a classificação. Todos os scripts Bash,
fontes Python e JSON do escopo passam por validação sintática. Tokens de acesso remoto,
energia, sessão, serviço ou varredura ampla de processos só podem existir nos validadores
estáticos revisados; nunca nos executores automáticos.

O runner de processos mantém descritores abertos dos namespaces originais. Assim, uma
variável de ambiente copiada ou falsificada não basta para liberar o teste. Dentro do
namespace, um watchdog registra PID+starttime do único filho e aplica limites de CPU,
memória, arquivo e tempo. TERM/KILL só podem alcançar esse filho e somente depois de
revalidar o starttime. Quando o PID 1 do namespace termina, o kernel remove qualquer
resto interno; não existe fallback para o host.

`test-tools.sh` prova status/log/manifesto, redação de credenciais conhecidas,
checkpoint append-only e restauração byte a byte de tracked, untracked e symlink.

Execução local canônica:

```sh
bash framework/tests/run-safe-gates.sh \
  --log-root /caminho/absoluto/para/logs
```

O resumo final precisa declarar `hardware_ran=0 device_access=0` e
`guest_initializers_executed=0`. Integração física não tem comando na matriz automática;
ela só pode ser criada numa etapa de device explicitamente autorizada e com log separado.

## Fechamento M20 e checkpoint verde

[`m20-closure-v1.json`](m20-closure-v1.json) liga M20-001..020 aos gates de sintaxe,
sanitizers/fuzz, componentes puros, fixtures sintéticas, NXExtract, NXRelease,
symlink/hardlink/TOCTOU, concorrência, semântica FAT/exFAT simulada, baixa glibc,
reprodutibilidade e logs duráveis. O gate é:

```sh
python3 -B framework/tests/test_m20_closure.py
```

O último comando do runner canônico é
[`capture-green-checkpoint.sh`](capture-green-checkpoint.sh). Como o runner usa
`set -e`, qualquer gate vermelho encerra o conjunto antes desse comando. Só um conjunto
integralmente verde cria o snapshot append-only de `framework/`,
`suportando_outros_devices/` e `publicando_ports/` fora do repositório. Isso não autoriza
hardware: todas as entradas `hardware` continuam manuais, sem comando e sem endereço.
