# nxbootstrap

`nxbootstrap` é a camada comum anterior ao `main` dos ports PortMaster. Ela gera um
wrapper visível curto, valida a ABI e os arquivos, prepara o ambiente do host, executa
NXExtract isoladamente e supervisiona uma única instância do loader.

O código de jogo, JNI, EGL, áudio e controle não pertence ao shell. Depois do
preflight, o comportamento multi-device fica no loader ligado a `nxcompat` e
`nxinput`.

Versão atual: `0.6.0`, manifesto canônico `schema_version: 2`.

## Limite pré-main

Antes que uma biblioteca estática dentro do loader possa executar, o sistema precisa
resolver:

- arquitetura do kernel/userland e `PT_INTERP`;
- loader dinâmico ARMHF ou AArch64;
- paths de bibliotecas do firmware, PortMaster e do port;
- `PORT_32BIT` que alguns firmwares detectam no wrapper;
- dados do dono que ainda precisam ser extraídos;
- lock, supervisão do filho direto, sinais e retorno ao frontend.

Essas responsabilidades são de `nxbootstrap`. Detecção gráfica, seleção real de
backend e quirks da engine só começam dentro do ELF.

## Fases de execução

### Histórico — biblioteca 0.5.1 aposentada

A biblioteca de runtime aposentada (`nxbootstrap.sh`, mantida na árvore apenas
como evidência de contrato; nunca mais gerada) tinha `nxbootstrap_main` nesta ordem:

1. valida toda a configuração, retira paths privados herdados e abre/rotaciona o
   log por descritor verificado;
2. entra no diretório real do port;
3. carrega `control.txt`, o `mod_${CFW_NAME}.txt` seguro e `get_controls`;
4. descobre um runtime de sessão existente e gravável;
5. valida loader, arquitetura e interpretador da ABI;
6. adquire lock fora do filesystem do jogo e falha fechado se ele estiver ocupado;
7. prepara o ambiente de bibliotecas do host e o mapping do PortMaster;
8. chama `pm_platform_helper` antes de qualquer UI/extractor;
9. executa NXExtract em primeiro plano e, depois, o prepare declarado;
10. verifica todos os arquivos obrigatórios;
11. acrescenta bibliotecas privadas e aplica `HOME` somente se solicitado;
12. inicia um único filho, repassa sinais, espera o status e chama `pm_finish` uma
    única vez.

Separar ambiente do host e ambiente privado é intencional. `pm_platform_helper` e
NXExtract precisam enxergar primeiro as bibliotecas do firmware/PortMaster; as `.so`
do jogo só entram depois, para não contaminar ferramentas de setup.

`nxbootstrap` não usa `setsid` e não solta o jogo sem supervisão. O shell permanece
como pai, mesmo que o loader seja iniciado em background internamente para permitir
traps e `wait` corretos.

## PortMaster

No produto final, PortMaster é obrigatório. Quando `control.txt` é encontrado,
`nxbootstrap`:

- carrega a integração do firmware;
- aceita a `controlfolder` canônica que o controle publicar;
- chama `get_controls` quando disponível e exporta apenas o mapping real recebido;
- passa o executável verdadeiro para `pm_platform_helper`, registrando uma falha
  opcional sem impedir o jogo;
- aceita `PM_PIPE` como sinal de handoff somente quando ele é um FIFO vivo e não um
  symlink; tipo inseguro, API ausente/falha ou FIFO persistente abortam antes da UI;
- chama `pm_finish` exatamente uma vez em sucesso, falha ou sinal.

A procura aceita raízes ArkOS, ROCKNIX, muOS, Knulli/Batocera, TrimUI, Miyoo,
NextOS, RetroDECK e uma raiz fornecida pelo ambiente. Arquivo de controle e mod não
podem ser symlinks. Um nome `CFW_NAME` não seguro é ignorado. O log registra a raiz
canônica, versões disponíveis, resultado do `get_controls`, tamanho do mapping e o
banco publicado, sem despejar o mapping inteiro.

O contrato completo e as fontes oficiais fixadas estão em
[`../portmaster`](../portmaster/README.md).

Se o PortMaster não existir, o bootstrap registra “standalone mode” para permitir
testes e diagnósticos. Isso não autoriza publicar um port sem testar o fluxo real do
PortMaster, controles e retorno ao frontend.

## Manifesto e gerador

O arquivo declarativo `nxport.json` elimina cópias manuais divergentes. Exemplo:

```json
{
  "schema_version": 2,
  "id": "jogo",
  "title": "Jogo",
  "launcher_name": "Jogo.sh",
  "architecture": "aarch64",
  "executable": "jogo-loader",
  "argument_mode": "game-dir-and-passthrough",
  "home_mode": "preserve",
  "nxextract": {"mode": "auto", "version": "1.2.6"},
  "required_files": ["jogo-loader"],
  "private_library_paths": ["libs"],
  "prepare_script": "",
  "required_capabilities": [
    "host.portmaster",
    "graphics.gles2",
    "input.controller-mapping"
  ],
  "enabled_quirks": [],
  "runtime_report": "log-and-logo"
}
```

Campos:

| Campo | Contrato |
| --- | --- |
| `id` | identificador minúsculo e seguro, até 63 caracteres |
| `launcher_name` | basename terminado em `.sh` |
| `architecture` | `armv7`, `aarch64`, `i386` ou `x86_64` |
| `executable` | caminho relativo normalizado dentro do port |
| `argument_mode` | nenhum, passthrough, game-dir, ou ambos |
| `home_mode` | `preserve` por padrão; `port` somente opt-in |
| `nxextract` | objeto com modo `auto`/`yes`/`no` e versão exata `1.2.6` |
| `required_files` | arquivos não vazios exigidos antes do launch |
| `private_library_paths` | diretórios privados relativos, somente na fase do jogo |
| `prepare_script` | fase Bash relativa, opcional e específica do jogo |
| `required_capabilities` | nomes exatos do registry finito; cada um só é satisfeito no nível de evidência declarado |
| `enabled_quirks` | quirks `adapter.*`, `engine.*` ou `game.*`, vazios por padrão |
| `runtime_report` | `log` ou `log-and-logo`; nunca pode desativar o diagnóstico |

O gerador rejeita campos desconhecidos, controles, paths absolutos/`..`, duplicatas,
paths pessoais, capability ausente do
[`capabilities-v1.json`](../nxcompat/capabilities-v1.json), seletores de device,
arquiteturas e modos inválidos. Nenhuma capability é obrigatória por padrão; nome de
firmware/aparelho e presença de arquivo não substituem o receipt exigido. O executável
entra automaticamente em `required_files`. Escritas são atômicas e arquivos
existentes não são sobrescritos sem `--force`. Nem `--force` autoriza uma raiz de
saída ou diretório do port que seja symlink; um target symlink é substituído como
entrada de diretório, nunca seguido.

Entrada legada `schema_version: 1` continua aceita apenas pelo gerador, que a atualiza
deterministicamente e grava sempre v2. Um release público aceita somente a saída v2
canônica; não existe downgrade silencioso. Os schemas estão em
[`schema`](schema/nxport-v2.schema.json), e o lock semântico das três camadas e dos
oito componentes está em
[`../contracts/declarative-v1.json`](../contracts/declarative-v1.json).

```sh
python3 framework/nxbootstrap/tools/generate-port.py \
  caminho/nxport.json --output caminho/do/pacote
```

O resultado é:

```text
Jogo.sh                        launcher Bash único e autocontido, modo 0755
jogo/nxport.json               manifesto canônico, modo 0644
```

Não existe biblioteca de runtime nem receipt: a 0.6.0 aposentou
`nxbootstrap-*.sh` e `nxdeployment.json` do produto gerado (forma Limbo — a
biblioteca bash gigante foi um modo de falha real em campo). Toda a lógica de
host/plano/negociação vive no loader C (`nxcompat`/`nxgl`/`nxinput`); o shell
faz apenas o contrato PortMaster e as garantias dos ports de ouro:

- cadeia de descoberta do `controlfolder` (5 raízes) → `control.txt` →
  `mod_${CFW_NAME}.txt` → `get_controls`, tudo opcional e fail-open;
- `GAMEDIR` derivado de `$directory` com fallback relativo por `readlink -f`;
- `log.txt` durável com rotação para `log.prev.txt`;
- **instância única** por `flock` persistente por port ID num diretório de runtime
  0700 fora do jogo, com identidade path/FD e link count validados: trocar o ELF
  por outro inode não abre uma segunda instância;
- NXExtract em foreground e `required_files` fail-closed antes do jogo,
  abortando com `pm_finish` em falha;
- `port-env.sh` opcional e não-symlink como único ponto de extensão por-port;
- jogo como filho direto supervisionado por PID + starttime: trap envia TERM,
  aguarda prazo finito, revalida a identidade antes de KILL e re-espera para
  devolver o status real; HUP/INT/TERM antes do jogo retornam 129/130/143 e
  `pm_finish` é protegido para executar exatamente uma vez;
- reset do console (`printf '\033c'` no `$CUR_TTY`) antes do `pm_finish`,
  cobrindo jogos que saem com o keymap em K_OFF.

## Diagnóstico

1. Nenhum `log.txt` novo: o launcher não chegou a redirecionar a saída —
   investigue entrada do frontend, `/bin/bash`, erro de parse, storage
   somente-leitura.
2. `log.txt` presente: o cabeçalho registra gerador/versão e `cfw=`; cada
   fase seguinte (extração, platform helper, jogo, status de saída) aparece em
   ordem no mesmo arquivo.
3. Loader iniciado: o runtime C assume o relato (probe/plano/backend do
   nxcompat), com os próprios marcadores.

## ARMHF literal e ABI

Para `architecture: armv7`, o wrapper visível gerado contém exatamente:

```sh
PORT_32BIT="Y"
export PORT_32BIT
```

O mesmo launcher exporta a variável usada pelo bootstrap. O bootstrap aceita kernel
ARMv7/ARMv8l ou AArch64, mas exige encontrar um interpretador ARMHF real. AArch64 exige userland/kernel
compatível e seu interpretador. Antes do launch, o bootstrap lê o header e os program
headers do ELF, confere class/little-endian/machine e exige exatamente o `PT_INTERP`
Linux canônico da ABI. Encontrar esse contrato correto ainda não prova que todas as
`DT_NEEDED` existem; a auditoria ELF integral do NXRelease continua obrigatória.

ARMv7 e AArch64 usam artefatos separados. Todo ELF Linux de um release público deve
ficar em `GLIBC_2.30` ou inferior, idealmente `GLIBC_2.17`.

## Bibliotecas e HOME

O `LD_LIBRARY_PATH` é montado sem duplicatas:

1. apenas na fase do jogo, paths privados declarados;
2. bibliotecas do PortMaster e da arquitetura correspondente;
3. diretórios do firmware correspondentes à ABI;
4. ambiente herdado, removendo qualquer path dentro do port;
5. diretórios genéricos do sistema.

Na fase de host/setup, o primeiro item é omitido desde antes de carregar o
`control.txt`. Diretórios privados são aceitos somente quando declarados e são
rejeitados se contiverem providers EGL, GL, GLES, GBM, DRM, Mali ou SDL.

`home_mode: preserve` é o padrão e deixa a sessão do firmware intacta.
`home_mode: port` exporta `HOME` para o diretório do jogo e deve ser escolhido
somente depois de confirmar onde a engine procura config/save. Save e cache permanecem
nos caminhos nativos da engine; qualquer redirecionamento adicional pertence ao adapter,
precisa ficar contido e ter prova própria. O manifesto genérico nunca escolhe save/cache
por firmware. O núcleo não inventa `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`, resolução ou
override Mesa.

## Lock e supervisão exata

O lock fica num namespace 0700 dentro do runtime de sessão ou num diretório privado
sob o tmp local, nunca no cartão/FAT/exFAT do jogo. `flock -n` é usado quando existe,
inclusive na forma limitada do BusyBox. Se `flock` não existe, o fallback `mkdir`
grava PID e starttime de `/proc`. Esse fallback nunca recupera automaticamente um
diretório existente: shell não oferece compare-and-swap seguro para distinguir um
lock órfão de um criador ainda entre `mkdir` e a gravação do owner. `HUP`, `INT`, `TERM`
e `EXIT` limpam normalmente; depois de um término não capturável, ele falha fechado até
uma manutenção explícita provar que o owner morreu. Nunca abre um segundo namespace.

Lock ocupado não autoriza o bootstrap a procurar ou encerrar a instância anterior. Ele
registra o conflito e aborta. Cwd, `comm`, cmdline, executável e ambiente são úteis para
diagnóstico operacional, mas não provam ownership suficiente para alimentar um sinal.

O bootstrap guarda somente o PID retornado por `$!` e seu starttime. Em
`HUP`/`INT`/`TERM`,
confirma ambos imediatamente antes de sinalizar esse filho direto, aguarda um prazo e
repete a confirmação antes do fallback terminal. Ele não enumera o namespace PID e não
tenta capturar descendentes por `/proc`. Uma fase ou engine que crie outros processos deve
supervisioná-los dentro do seu próprio adapter, continuar em foreground e só retornar quando
eles acabarem.

As suítes que criam processos também falham fechado. `tests/test-nxbootstrap.sh` e
`tests/test-generator.sh` recusam execução direta com status 77. O único entry point é
`tests/run-isolated.sh`, que exige `unshare` de user, PID e mount namespace com `/proc`
privado; se o kernel/firmware não oferecer isso, a suíte é pulada e nunca cai no host.
Descritores abertos dos três namespaces originais impedem liberar o teste apenas forjando
variáveis de ambiente. O processo interno precisa ser PID 1 e um watchdog limita CPU,
memória, arquivos, número de processos e tempo de parede. Ele registra PID+starttime do
único filho e só pode enviar TERM/KILL a esse filho depois de revalidar ambos. Quando o PID
1 privado termina, `--kill-child=KILL` contém qualquer resto no próprio namespace.
`tests/test-safety-static.sh` não cria processos do port e pode rodar diretamente.

Sinais `HUP`/`INT`/`TERM` param o filho supervisionado. O status do jogo é preservado e
`pm_finish`/lock são finalizados uma vez. O pedido `SELECT/BACK + START` de `nxinput`
deve entrar no shutdown seguro da engine; ele não deve chamar `kill` por fora do fluxo
nativo.

## NXExtract 1.2.6

NXExtract continua separado porque sua responsabilidade é preparar dados do dono,
não adaptar a engine. Para `nxextract.mode: yes` ou para `auto` com receita presente,
o port precisa vendorizar juntos:

- `extractor.json` regular e não-symlink;
- `nxextract/run-extractor.sh` regular e não-symlink;
- `nxextract/nxextract.py` e `nxextract/nxextract-runtime-env.sh` do conjunto
  canônico `1.2.6`.

O runner é chamado explicitamente com `bash`, portanto funciona mesmo se ZIP/FAT
perder o bit executável. Ele roda em primeiro plano, com o diretório do jogo e o path
de bibliotecas do firmware informados, antes do prepare e do loader. Uma falha aborta
o port e aponta para o diagnóstico do extractor; a engine nunca deve concorrer com
cópia, backup ou rollback dos dados.

NXExtract não deve ser ligado dentro de `nxcompat`, e o loader não deve reimplementar
seu journal/rollback.

## Integração com o loader

O bootstrap exporta:

- `NXCOMPAT_PORT_ID`;
- `NXCOMPAT_GAME_DIR`;
- `NXCOMPAT_REQUIRED_CAPABILITIES`;
- `NXCOMPAT_ENABLED_QUIRKS`;
- `NXCOMPAT_RUNTIME_REPORT`;
- `SDL_GAMECONTROLLERCONFIG` somente quando `get_controls` devolveu valor real;
- `PORT_32BIT=Y` no fluxo ARMHF.

O loader usa os dois primeiros no `nxcompat_probe`, deixa `nxcompat` negociar vídeo e
áudio por abertura real e cria `nxinput` depois que o mapping herdado está visível.
O shell não precisa crescer quando uma engine exige um quirk: esse código pertence à
ponte compilada e deve ser protegido pela capacidade/evidência correspondente.

## Testes

```sh
bash framework/tests/run-safe-gates.sh \
  --log-root /caminho/absoluto/para/logs
```

A bateria do gerador verifica validação, não-overwrite, determinismo, output/target
symlink, sintaxe, relocação por symlink, argumentos, log exclusivo e o literal ARMHF.
A bateria do lifecycle usa um PortMaster e loader sintéticos para verificar ordem de
fases, mapping, `HOME` opt-in, NXExtract sem bit executável, prepare, ELF/PT_INTERP
sintético, hardlinks, HUP, helper, `pm_finish`, status do filho e lock fora do filesystem
do jogo. O ledger [`m06-audit-v1.json`](m06-audit-v1.json) liga os 30 requisitos M06 a
seus tokens de implementação e ataques. O teste de supervisão usa `/proc` sintético e um sink de sinal; o único
sinal real da infraestrutura é exercitado contra um filho exato dentro do namespace
selado. A matriz e os detalhes estão em [`../tests`](../tests/README.md).

Esses testes não substituem o gate no aparelho. Antes do release, confirme via
PortMaster que não existe instância anterior, o logo mostra capacidades/backends,
vídeo e som entram automaticamente, controles e hotplug funcionam, save/reload
persistem e a saída devolve o frontend sem processo residual.
