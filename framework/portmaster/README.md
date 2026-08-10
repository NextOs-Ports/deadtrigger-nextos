# Contrato PortMaster do framework universal

Este diretório fixa a fronteira entre um launcher pequeno e a integração real do
PortMaster. O princípio é **detectar primeiro, corrigir depois e nunca forçar algo
por padrão**.

O contrato não transforma um port comprovado num device em evidência universal. Ele
padroniza descoberta, ordem de inicialização, controles, ABI, instalação, cleanup e
segurança. Vídeo, áudio e quirks continuam dependentes de capacidades medidas e dos
gates físicos de cada combinação.

## Fontes reproduzíveis

[`upstream-sources-v1.json`](upstream-sources-v1.json) fixa commits e SHA-256 de:

- PortMaster-GUI `8f9ddc4b0f75dfe61eb370bd3d1b4ec9d5ef6967`, usado para runtime,
  `control.txt`, `funcs.txt`, mods e HarbourMaster;
- PortMaster-New `df9805908db41b32c18532e78b9844ebe8c0a768`, usado para a política
  atual de submissão e runtimes;
- snapshots oficiais de `funcs.txt` v1, v2 e v3;
- integração NextOS `2026.07.05-0600`, exclusivamente como evidência local e como
  fonte negativa do hook global legado.

Nenhum script de jogo é fixture do marco M03. WIP é proibido como fonte positiva.
Pikmin, PartyBoard, GTA, Bully, Dysmantle, LIMBO e Chrono Trigger permanecem
referências limitadas ao que foi realmente provado; não demonstram compatibilidade
universal.

As regras de máquina ficam em [`contract-v1.json`](contract-v1.json), e os dez
cenários reproduzíveis em
[`fixtures/contract-cases-v1.json`](fixtures/contract-cases-v1.json).

## Ordem obrigatória

O fluxo anterior ao jogo é:

1. declarar ABI e o literal `PORT_32BIT="Y"` no wrapper ARMHF;
2. encontrar um `control.txt` regular e carregá-lo;
3. aceitar a `controlfolder` canônica publicada pelo próprio controle;
4. carregar `mod_${CFW_NAME}.txt` somente com nome sanitizado e arquivo regular,
   não-symlink;
5. chamar `get_controls` se existir, sem inventar GUID, VID/PID ou layout;
6. validar ELF, arquitetura e linker dinâmico;
7. preparar paths do host e chamar `pm_platform_helper` no máximo uma vez, com o
   executável real;
8. executar NXExtract e o prepare declarado em primeiro plano;
9. adicionar dependências privadas declaradas, iniciar um filho direto e aguardá-lo;
10. chamar `pm_finish` exatamente uma vez e devolver o status do jogo.

`pm_platform_helper` é uma dica de plataforma, não um launcher. Falha dele é
registrada e o fluxo continua. Falha de `pm_finish` também é registrada, sem
substituir o status real do jogo.

## Descoberta das raízes

`nxbootstrap` tenta primeiro uma `controlfolder` válida já publicada. Depois procura
raízes por capacidade:

| Família observada | Raízes candidatas relevantes |
| --- | --- |
| ArkOS/genérica | `/roms/tools/PortMaster`, `/roms2/tools/PortMaster` |
| ROCKNIX/JELOS | `/opt/system/Tools/PortMaster`, `/storage/.config/PortMaster` |
| muOS | `/mnt/mmc/MUOS/PortMaster` e raízes publicadas pelo controle |
| Knulli/Batocera | `XDG_DATA_HOME/PortMaster`, `/userdata/system/.local/share/PortMaster` |
| TrimUI/Miyoo | raízes próprias sob `/mnt/SDCARD` ou `/mnt/sdcard` |
| NextOS | `/storage/roms/ports/PortMaster` |
| RetroDECK | raiz de dados do Flatpak publicada no ambiente |

Esses nomes servem para descoberta e log. Eles não selecionam Mesa, EGL, renderer,
backend SDL, áudio ou uma correção de engine.

## Compatibilidade das APIs

`control.txt` não expõe um número de versão estável. `PM_FUNCS_VERSION` existe, mas
o framework testa cada função antes de usá-la:

| `funcs.txt` | Base observada | Não garantido |
| --- | --- | --- |
| v1 | `bind_directories`, `pm_finish` | `bind_files`, `pm_message`, `pm_platform_helper` |
| v2 | v1 + `pm_message`, cleanup GPTOKEYB | `bind_files`, `pm_platform_helper` |
| v3 | `bind_directories`, `bind_files`, `pm_message`, `pm_platform_helper`, `pm_finish` | — |

O mapping gerado pelo `get_controls` ativo é a autoridade. Mapping vazio ou falha
vira diagnóstico; o shim preserva o ambiente do firmware e não cria um banco de
controles por device.

## ABI, bibliotecas e filesystem

`PORT_32BIT=Y` declara ao scanner/mod que o port já é ARMHF. Não converte um ELF nem
prova o linker `/lib/ld-linux-armhf.so.3`. AArch64 exige
`/lib/ld-linux-aarch64.so.1`; os targets usam fonte/API comum e artefatos separados.

Na fase do jogo, a ordem é: paths privados declarados, bibliotecas do PortMaster,
paths da ABI do firmware, paths herdados que não pertencem ao port e paths genéricos.
Na fase de setup não entram bibliotecas privadas. Provedores privados de EGL, GL,
GLES, GBM, DRM, Mali ou Mesa são rejeitados.

FAT/exFAT não é tratado como POSIX. Helpers vendorizados são chamados por `bash`, o
lock fica no runtime/tmp privado e um bind só é usado se o port precisar dele e a
função existir. Não se pressupõe `chmod` persistente, symlink ou `flock` nos dados.

## Instalação e saída

HarbourMaster atualiza metadados antigos, mas o formato atual fixado é `port.json`
v4. Um item terminado em `/` representa diretório. O launcher `.sh` de topo vai
para `scripts_dir`; os demais membros vão para `ports_dir`. Caminhos absolutos e
travessia por `..` são rejeitados, e permissões são reparadas após a extração.

O shim não administra frontend. Não chama `systemctl`, `loginctl`, `setsid`, logout,
bloqueio, suspensão, reboot ou poweroff. Sinais alcançam somente o PID filho direto,
confirmado por PID e starttime. Ao terminar, o launcher deixa o supervisor existente
cuidar do frontend.

## Evidência negativa NextOS

O hook local legado `portmaster_compatibility.sh` reescreve todos os launchers,
injeta providers SDL por device e remove bibliotecas de diretórios de ports. Ele está
registrado como **negative-only**, com reutilização proibida. O substituto correto é
um ajuste process-local, aplicado somente ao port atual depois de uma falha medida.

O launcher local do PortMaster também pode reiniciar `emustation.service`; isso não
é responsabilidade do framework e não será chamado durante os gates do shim.

## Gate estático

O teste não cria processos de jogo, não acessa aparelhos e não altera o host:

```sh
python3 framework/portmaster/tests/test_portmaster_contract.py
```

Ele valida fontes e hashes, fixtures, APIs v1–v3, ordem do bootstrap, roots,
instalação v4, ARMHF, FAT/exFAT, ausência de overrides de vídeo/áudio e ausência de
comandos de processo, sessão, serviço ou energia.
