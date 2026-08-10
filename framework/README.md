# Framework universal de ports

Este diretório concentra o contrato comum para ports novos usados pelo
PortMaster. O objetivo é manter um único launcher visível pequeno, mover
comportamento repetido e testável para componentes compartilhados e reservar para
cada jogo apenas a ponte que realmente depende da engine.

“Universal” aqui significa **uma arquitetura e uma API comuns**, não um único ELF
capaz de executar qualquer ABI nem uma promessa antecipada sobre todo firmware.
Cada loader continua sendo compilado para sua arquitetura real, e cada combinação
device/firmware precisa passar pelos gates físicos antes de ser anunciada como
suportada.

O framework está na série inicial `0.x`. Ele deve entrar primeiro em ports
piloto, sem migração em massa de ports aprovados.

O fluxo de produto tem duas fases separadas. `rad <jogo>` fecha primeiro o port
somente no Mali-450 autorizado; consultar ou reutilizar uma peça deste framework
nessa fase não cria suporte universal. Depois desse baseline estar completo, apenas
o pedido explícito `radu <jogo>` abre a fase universal, na qual esta árvore passa a
reger scaffold, compatibilidade, empacotamento e evidência multi-device.

## Catálogo e limite de suporte

O [`catalog`](catalog/README.md) registra cada referência por tag, commit, pacote,
SHA-256, engine, ABI e evidência física. A matriz expandida possui 42 decisões por
entrada e falha se um campo desaparecer.

O catálogo separa explicitamente um framework universal de um port universal:
Pikmin, PartyBoard e os GTA, por exemplo, continuam referências limitadas aos
devices e subsistemas realmente provados. Nenhuma solução deles vira default ou
suporte multi-device por compartilhar engine, loader, GPU ou firmware.

## Arquitetura

```text
wrapper POSIX gerado
        |
        v
launcher único 0.6.0 (Bash, antes do ELF)
  PortMaster (fail-open) -> GAMEDIR físico -> log.txt -> executável validado
  -> flock de instância -> NXExtract -> arquivos obrigatórios -> exports
  NXCOMPAT_* -> filho supervisionado (status real) -> console reset -> pm_finish
        |
        v
loader ARMv7 ou AArch64
  nxloader + nxcompat + nxinput + nxandroid, ligados estaticamente
        |
        v
ponte específica da engine
  JNI/Android, lifecycle, EGL/GLES, áudio, save e shutdown nativos
```

As camadas têm fronteiras deliberadas:

- [`nxbootstrap`](nxbootstrap/README.md) resolve o que precisa acontecer antes de
  o kernel carregar o executável: PortMaster, interpretador da ABI, caminhos de
  bibliotecas, instância única, extração dos dados do dono e supervisão;
- [`nxgenerator`](nxgenerator/README.md) fecha M19 gerando o projeto completo a
  partir de um manifesto: launcher único, bootstrap e NXExtract pinados,
  metadata PortMaster, adapter deliberadamente vazio e README bilíngue;
- [`nxcompat`](nxcompat/README.md) é C99 estático dentro do loader. Ele mede
  capacidades, planeja somente ajustes seguros, negocia backends por tentativa
  real e relata o que ocorreu;
- [`nxaudio`](nxaudio/README.md) mantém a fila PCM realtime/worker, lifecycle,
  razões de falha e contratos finitos por stack; a abertura real continua no
  `nxcompat` e cada mixer/callback permanece no adapter comprovado;
- [`nxinput`](nxinput/README.md) é C99/SDL2 estático dentro do loader. Ele
  normaliza até quatro controles, hotplug, focus, deadzone, taps curtos, pedido de
  saída e cursor contextual;
- [`nxloader`](nxloader/README.md) carrega ELFs Android ARMv7/AArch64 com
  relocações, imports, W^X, constructors e `JNI_OnLoad` explícitos;
- [`nxandroid`](nxandroid/README.md) valida o catálogo Bionic/JNI/NDK e a ordem
  declarada pelo adapter sem fornecer uma VM falsa, callbacks de engine ou
  teardown genéricos;
- [`portmaster`](portmaster/README.md) fixa descoberta, versões das APIs,
  controles, instalação, filesystem e lifecycle usando fontes oficiais e
  integrações locais explicitamente limitadas;
- [`contracts`](contracts/README.md) congela as três
  camadas, versões/APIs dos componentes, schema `nxport` v2, compatibilidade v1 e
  códigos de saída;
- [`catalog`](catalog/README.md) é a fronteira de evidência: WIP fornece apenas
  alertas; fixes, relatos da comunidade e sessões físicas permanecem níveis
  diferentes;
- [`nxabi`](nxabi/README.md) fecha M17 com pins offline, inventário de todos os
  ELFs, tetos GLIBC/GLIBCXX/CXXABI e builds determinísticos ARMHF/AArch64;
- [`nxrelease 0.2.5`](nxrelease/README.md) fecha M18 com stage allowlist, auditoria
  adversarial, licença obrigatória, ZIP/SBOM determinísticos e publicação sem
  overwrite;
- [`tests`](tests/README.md) fecha M20: classifica efeitos, obriga logs, recusa
  processos fora do user/PID/mount namespace selado e só cria checkpoint depois
  de todo o conjunto automático ficar verde;
- NXExtract `1.2.6` permanece um componente separado e vendorizado no port. Ele
  roda em primeiro plano, numa fase isolada, antes de qualquer thread da engine;
- a ponte da engine preserva a sequência nativa do jogo. Assinaturas JNI,
  `init_array`, `JNI_OnLoad`, criação do contexto, callbacks, saves e teardown não
  podem ser substituídos por um fluxo genérico.

## Por que existem duas partes

Uma biblioteca compilada só começa a executar depois que o kernel encontrou o
`PT_INTERP`, o carregador dinâmico aceitou a ABI e todas as dependências iniciais
foram resolvidas. Portanto, ela não consegue corrigir o próprio interpretador,
`DT_NEEDED`, uma glibc incompatível, `LD_LIBRARY_PATH`, a marca `PORT_32BIT` ou a
ausência dos dados extraídos.

Esses problemas ficam em `nxbootstrap`. Depois que o loader entra em `main`,
`nxcompat` assume a detecção e o comportamento compartilhado. Fundir as duas fases
criaria novamente um `run.sh` grande e, ainda assim, não eliminaria o limite do
carregador dinâmico.

Desde o `nxbootstrap 0.6.0` o produto gerado é **um único launcher autocontido**:
a biblioteca bash de runtime (`nxbootstrap-*.sh`) e o receipt `nxdeployment.json`
foram aposentados. Um update por overlay pode deixar arquivos dessas versões
antigas no cartão; eles são inertes — o launcher novo aponta explicitamente para
o executável versionado e não carrega biblioteca nenhuma. Clean install e update
overlay continuam gates separados.

O diagnóstico tem três fronteiras: nenhum `log.txt` novo significa que o launcher
não chegou a redirecionar a saída (entrada do frontend, `/bin/bash`, parse,
storage somente-leitura); `log.txt` presente registra gerador/versão, `cfw=` e
cada fase (extração, platform helper, jogo, status de saída) em ordem; e o
runtime C (`nxcompat`) assume o relato dali em diante com marcadores próprios.
Nenhum script pode criar log antes de o shell executá-lo. Um filesystem
cheio/somente leitura, erro de interpretador/sintaxe, SIGKILL ou perda de energia
também podem impedir a evidência; a
ausência de log sozinha nunca prova que o launcher não foi invocado.

## Princípios obrigatórios

### Universalização framework-first e evolução dirigida pelos jogos

Quando `radu` abre a fase universal, o projeto usa o `nxgenerator`; um port já
aprovado compara seu baseline Mali-450 com os componentes atuais e adota somente as
camadas compatíveis que tragam ganho comprovável. A migração é incremental: não se
reabre comportamento verde e não se reescreve um adapter apenas para uniformizar a
árvore. Na fase anterior, `rad`, uma camada comum pode ser reutilizada se ajudar o
Mali-450, mas não se executa a matriz multi-device nem se presume suporte universal.

Quando um jogo expõe uma necessidade reutilizável, a correção entra no componente comum
junto com contrato/API, versão quando aplicável, documentação e regressão. Lifecycle,
JNI, offsets, patches, formatos e quirks particulares permanecem no adapter, opt-in e
desligados por padrão. Hipótese, workaround de WIP ou seleção por nome de device nunca
vira default global.

### Mali-450 e GLES2 são a base

O piso gráfico é o Mali-450 com GLES2 real. Um port público não pode exigir
Mesa, anunciar GLES 3.2 por variável, escolher uma resolução fixa ou habilitar uma
feature porque outro device a oferece. GLES3, ETC2, ASTC, NPOT completo e quirks de
compositor só entram atrás de uma capacidade medida no contexto real e de evidência
do jogo.

Uma correção específica deve ser estreita, nomeada, desligada por padrão e morar
na ponte que conhece a engine. O núcleo não altera globalmente sampler/wrap,
formato de textura, alpha do backbuffer, estado GL ou dono do present.

### Capacidade primeiro

Modelo e nome do firmware servem para diagnóstico, não como seletor principal.
Decisões devem partir de fatos como:

- arquitetura do processo e do kernel;
- interpretador e bibliotecas da ABI realmente presentes;
- socket de sessão, Wayland, DRM conectado ou framebuffer;
- socket Pulse/PipeWire e dispositivo ALSA;
- driver SDL que realmente abriu;
- vendor, renderer, versão, extensões e atributos do drawable real;
- memória e tipo do filesystem onde o port está.

Uma tabela por nome de device só é aceitável como exceção comprovada quando
não existe uma capacidade observável equivalente.

### O ambiente herdado tem prioridade

PortMaster e firmware conhecem a sessão que criaram. O fluxo de vídeo e áudio é:

```text
ambiente herdado -> autodetecção normal -> descoberta exposta pelo runtime
```

Cada passo só é abandonado depois de uma falha real. Não há lista fixa de
`SDL_VIDEODRIVER`/`SDL_AUDIODRIVER`, card ALSA, display, resolução ou versão Mesa.
No fallback final, o adapter pode testar apenas nomes enumerados pela biblioteca
que está executando, validando uma saída real e rejeitando backends nulos.

### O PortMaster é parte do produto

Um release real deve passar por `control.txt`, `get_controls`,
`pm_platform_helper` e `pm_finish`. O modo standalone existe para desenvolvimento,
diagnóstico e testes, mas não substitui o gate PortMaster no aparelho.

O mapping herdado continua sendo a autoridade sobre os botões físicos.
`nxinput` apresenta posições Xbox estáveis à ponte; ele não grava GUIDs ou layouts
genéricos. D-pad e A permanecem nativos. Quando uma UI touch precisa de cursor, o
analógico direito move uma seta polida e R3 clica somente no contexto de menu; em
gameplay ambos retornam à função original.

### ABI e release público

ARMv7/ARMHF e AArch64 compartilham fonte e contratos, mas geram binários distintos.
O wrapper ARMHF contém literalmente `PORT_32BIT="Y"` para a varredura estática de
firmwares que dependem disso.

Todo ELF Linux construído pelo projeto e incluído num pacote público deve exigir no
máximo `GLIBC_2.30`; o alvo preferido é `GLIBC_2.17`. O gate vale para todos os
executáveis e `.so` do ZIP, não apenas para o loader principal.

Os recibos `nxinput/references/m15-input-contract-v1.json`,
`nxandroid/references/m16-adapter-contract-v1.json`,
`nxabi/m17-closure-v1.json`, `nxrelease/m18-closure-v1.json`,
`nxgenerator/m19-closure-v1.json` e `tests/m20-closure-v1.json` fecham M15–M20
sem misturar níveis de prova. Aceitação humana já consolidada dos ports aprovados
não é reatribuída a automação; input virtual não é chamado de físico; e nenhum
offset, callback, JNI, lifecycle, save ou shutdown específico vira default do
framework. Combinações novas de port/device continuam exigindo aceitação própria.

## Fluxo da fase universal de um port

Este fluxo só começa após o port ter sido aprovado no Mali-450 e o usuário enviar
`radu <jogo>`.

1. Preservar como baseline a versão que executa 100% no Mali-450; um WIP serve
   apenas para registrar caminhos que falham.
2. Escrever o manifesto `nxproject` v1 e gerar a árvore universal pelo `nxgenerator`; ele
   materializa o `nxport.json` v2 e o launcher visível único (0.6.0) — sem
   biblioteca de runtime e sem receipt.
3. Fornecer a receita do jogo para o gerador vendorizar o conjunto NXExtract
   `1.2.6` exato quando houver dados do dono a preparar.
4. Ligar `nxcompat` e `nxinput` estaticamente em cada loader ARMv7/AArch64.
5. Implementar somente a ponte nativa da engine e quirks comprovados do jogo.
6. Mostrar no logo as linhas de device/capacidades, ajustes e backends realmente
   selecionados. A mesma informação deve ir para o log.
7. Auditar ABI, `PT_INTERP`, `DT_NEEDED`, RPATH/RUNPATH e versões GLIBC de todos os
   ELFs do pacote.
8. Testar boot completo, vídeo, áudio, controles, hotplug/focus, save/reload,
   suspend quando aplicável e saída limpa pelo PortMaster.

O `home_mode` padrão é `preserve`. Trocar `HOME` para o diretório do port é uma
decisão opt-in no manifesto, somente para jogos cuja localização de config/save foi
confirmada. Um comportamento conveniente em um jogo não vira política global.

## Status no logo

`nxcompat` emite mensagens por callback, sem impor SDL, fonte ou engine. O loader
pode guardá-las enquanto cria sua tela e desenhá-las sobre o logo assim que houver
um drawable. A sequência esperada é:

- identificação e capacidades medidas;
- ajustes process-local planejados/aplicados;
- vídeo e áudio que realmente abriram, ou aviso claro de áudio mudo;
- renderer/GLES/drawable depois da criação do contexto real.

O texto não deve expor hostname, endereço de rede, credencial ou caminho pessoal.
Ele informa o que aconteceu; não deve esconder falhas por trás de um nome de device.

## O que não pertence ao núcleo

- patches JNI e thunks soft-float específicos;
- ordem de inicialização ou shutdown da engine;
- conversão de touch, keycode ou estrutura HID particular;
- shader translation e correções de textura sem prova;
- caminhos de dados, nome de `.so`, argumentos e saves do jogo;
- overrides Mesa/GLES, backends SDL ou resoluções escolhidos por tabela;
- publicação automática antes dos testes físicos.

Essa separação é o que permite reaproveitar a base sem transformar um workaround
local em regressão para todos os outros ports.
