# nxinput

`nxinput` é a camada estática de controle para loaders novos do NextOS/PortMaster. Ela
abre até quatro `SDL_GameController`, mantém cada jogador pelo
`SDL_JoystickInstanceID` e oferece um estado Xbox pequeno e previsível para a ponte
específica da engine.

Ela não conhece Mali, Mesa, framebuffer, áudio, JNI ou uma engine particular. Também
não substitui o fluxo nativo do jogo. Sua única fronteira é:

```text
mapping do PortMaster/firmware -> SDL_GameController -> nxinput -> ponte da engine
```

## Contrato

- C99 e APIs antigas do SDL2 GameController; `SDL_CONTROLLERDEVICEREMAPPED` só é
  compilado quando o header oferece SDL 2.0.4 ou superior;
- biblioteca `STATIC`, destinada a entrar no loader ARMv7/AArch64 em vez de adicionar
  mais um processo ou helper ao port;
- no máximo quatro pads independentes, nunca fundidos no Player 1;
- `SDL_GAMECONTROLLERCONFIG_FILE` é carregado explicitamente para cobrir SDLs
  antigas, e `SDL_GAMECONTROLLERCONFIG` é reaplicado por último. O mapping direto
  do PortMaster mantém prioridade sobre o banco; PortMaster/CFW continua sendo a
  autoridade sobre o layout físico;
- hotplug por evento, com rescan periódico como recuperação caso o loop perca um evento;
- desconexão e perda de foco zeram botões, sticks, gatilhos e movimento do cursor. Os
  releases correspondentes ficam consumíveis, e presses ainda pendentes são descartados
  para não reaparecerem como ações fantasmas;
- deadzone radial com rescale e histerese separada para entrar/sair do neutro;
- todo tap observado por evento deixa um latch de press, mesmo se o botão já estiver
  solto quando a engine fizer seu próximo poll;
- `BACK/SELECT + START` apenas cria um pedido sticky de quit. O port decide quando
  executar pause/save/flush/teardown e nunca é encerrado pela biblioteca;
- cursor opcional, normalizado em `0..1`, progressivo, suavizado e baseado em tempo por
  frame. Ele usa exclusivamente stick direito + R3 e somente no contexto `MENU`.
  D-pad e A nunca fazem parte do cursor.

`nxinput` fica silencioso por padrão e não abre `/dev/input/eventN`, `jsN`, `gptokeyb`
ou `uinput`.

## Receipt M15

O contrato verificável dos 22 itens do M15 está em
`references/m15-input-contract-v1.json`. O gate puro pode ser executado com:

```sh
python3 -B framework/nxinput/tests/test_m15_input_contract.py
```

Ele aceita como evidência positiva somente Bully 2, Sonic 4 EP2, Horizon Chase,
KOTOR e ASM2 1.2.7, mantém mapping, offsets, keycodes, touch, callbacks e
shutdown específicos no adapter e rejeita dados privados e fontes não aprovadas.
O estado é `closed_for_framework`: os 22 itens têm contrato, teste e fronteira
adapter-specific explícitos.

`references/m15-runtime-receipt-v1.json` separa três níveis que não podem ser
confundidos: a aceitação humana anterior dos cinco ports finalizados, observações
físicas no GO-Super e observações virtuais de hotplug/desconexão. O recibo não
afirma que input virtual é controle físico e não atribui ao agente uma nova
validação manual de gameplay. Uma combinação nova de aparelho/firmware continua
precisando de aceitação própria antes de virar promessa pública; isso é gate da
release, não pendência do contrato M15.

## Receipt forte para nxcompat

`nxinput_nxcompat_publish_context()` recebe somente um `nxinput_context` opaco já
criado. A existência desse contexto representa o contrato concluído de subsistemas
GameController ativos, mappings herdados aplicados, scan inicial executado e event
watch habilitado. A ponte ainda consulta `nxinput_connected_count()` e os quatro
slots com `nxinput_get_pad()`, rejeitando divergência entre a contagem agregada e os
slots.

O receipt publicado contém apenas flags finitas, quantidade conectada e uma geração
de topologia derivada das gerações dos quatro slots. Nome do controle, GUID,
`instance_id`, botões e eixos não são copiados. Um pad realmente aberto comprova que
há mapping SDL em runtime; sem pad, a ponte não inventa mapping e preserva uma
observação independente que o probe já tenha recebido do PortMaster/banco.

A ponte não chama `nxinput_create()`, não drena eventos, não faz poll, não abre um
device e não simula hotplug. O loader a chama depois da criação e novamente após uma
mudança de topologia observada no seu fluxo normal. Publicar a mesma geração outra
vez é rejeitado como stale; falha nunca altera o registry anterior.

`input.hotplug` exige simultaneamente event watch **e** rescan ativo. O contexto
opaco atual prova o watch, mas não expõe se `rescan_interval_ms` foi desabilitado;
por isso esta bridge fica fail-closed e não anuncia `RESCAN_ACTIVE` nem satisfaz
`input.hotplug`. O publisher genérico aceita o receipt completo de um adapter que
tenha ambas as provas. Um accessor futuro do estado interno poderá elevar a bridge
sem transformar a configuração default em suposição universal.

## PortMaster e mapping

O launcher continua carregando `control.txt`/`get_controls` quando disponíveis e só
exporta um valor real:

```sh
[ -n "${sdl_controllerconfig:-}" ] &&
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
```

Isso precisa ocorrer antes do loader iniciar. Não grave GUID, índice, nome de device ou
um mapping genérico dentro de `nxinput`. Um fallback local pertence ao port e somente a
um GUID/layout fisicamente comprovado.

## Integração no loop

```c
nxinput_config input_config;
nxinput_context *input;

nxinput_config_init(&input_config);
input = nxinput_create(&input_config);
if (!input) {
    fprintf(stderr, "input: %s\n", SDL_GetError());
    return 1;
}

while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        nxinput_observe_event(input, &event);

        /* Obrigatório: nxinput só observa. O port ainda entrega o MESMO evento
         * à janela, lifecycle e handlers nativos da engine. */
        game_observe_event(&event);
        if (event.type == SDL_QUIT)
            request_safe_shutdown();
    }

    nxinput_poll(input);
    if (nxinput_quit_requested(input))
        request_safe_shutdown();

    /* A ponte deste jogo lê nxinput_get_pad() e converte o contrato Xbox para
     * callbacks JNI, HID, keycodes Android ou estruturas internas reais. */
    update_game_input(input);
}

nxinput_destroy(input);
```

Não chame `SDL_PollEvent` dentro de outra camada de input: só o loop dono da aplicação
drena a fila. Para uma engine que gerencia focus fora dos eventos SDL, chame
`nxinput_set_focus(input, 0/1)` no ponto equivalente do lifecycle.

### Slots, hotplug e multiplayer

`nxinput_get_pad(input, slot, &state)` expõe `connected`, `instance_id` e
`generation`. O slot é armazenamento local; `instance_id` identifica o pad naquela
conexão. A geração muda em connect/disconnect, permitindo que a ponte anuncie o
lifecycle no momento nativo correto da engine.

Em multiplayer, percorra os quatro slots e preserve cada identidade. Em jogo
single-player, `nxinput_first_connected()` encontra o primeiro slot vivo. Nunca reutilize
um índice antigo recebido em `SDL_CONTROLLERDEVICEADDED`: naquele evento o campo é um
índice de abertura; depois de aberto, a identidade é o instance ID.

### Polling e latches

O estado atual fica em `state.buttons` e nos seis floats normalizados. Eixos de stick
usam `-1..1`, com Y no sentido SDL (positivo para baixo); gatilhos usam `0..1`. Inverta Y
somente na ponte cuja engine realmente exigir.

Para não perder um toque de 10 ms numa engine a 30 Hz:

```c
uint32_t press = nxinput_consume_pressed(input, slot,
                                         NXINPUT_BUTTON_MASK_ALL);
uint32_t release = nxinput_consume_released(input, slot,
                                            NXINPUT_BUTTON_MASK_ALL);
dispatch_edges_to_engine(press, release);
```

O consumo é seletivo por máscara e não muda `state.buttons`. Down/up repetidos são
deduplicados. Num boundary de focus/hotplug, releases são preservados e presses pendentes
são limpos por segurança.

### Cursor contextual

O port deve habilitá-lo por estado real da UI, nunca por temporizador:

```c
nxinput_cursor_state cursor;

nxinput_set_cursor_context(input, menu_open ? NXINPUT_CURSOR_MENU
                                             : NXINPUT_CURSOR_GAMEPLAY);
nxinput_cursor_update(input, slot, frame_seconds, &cursor);
if (cursor.active) {
    draw_polished_arrow(cursor.x, cursor.y); /* asset/seta do port */
    if (nxinput_cursor_consume_click(input, slot))
        send_paired_touch_at(cursor.x, cursor.y);
}
```

O latch do clique do cursor é separado do latch normal de R3: observar/consumir um não
rouba o outro. Em gameplay, a API de cursor para completamente e a ponte continua
entregando stick direito/R3 à câmera ou ação original. O desenho da seta e o touch
DOWN/UP pareado pertencem ao port, pois dependem do drawable e da API real da engine.

## Build e teste

Standalone:

```sh
cmake -S framework/nxinput -B build/nxinput \
  -DNXINPUT_BUILD_TESTS=ON \
  -DNXINPUT_BUILD_NATIVE_TESTS=OFF \
  -DNXINPUT_WITH_NXCOMPAT=ON
cmake --build build/nxinput
ctest --test-dir build/nxinput --output-on-failure
```

Como subdiretório do loader:

```cmake
add_subdirectory(path/to/framework/nxinput)
target_link_libraries(game-loader PRIVATE nxinput)
```

Esse comando é o gate hermético M12: executa o gate estático e o teste separado de
`nxinput_nxcompat`, com contexto e quatro slots inteiramente fake. Ele valida
conexão/desconexão, geração monotônica, hotplug fail-closed sem prova de rescan e
agregação sem identificadores. Não chama `nxinput_create()`, não enumera SDL nem acessa
`/dev`, rede, sessão ou controle físico.

`NXINPUT_BUILD_NATIVE_TESTS=ON` habilita uma bateria separada/manual que liga o core
ao subsistema SDL nativo e pode enumerar GameController. Ela cobre extremos dos eixos,
gatilho, deadzone, latches, lifecycle, cursor e, quando suportado, um GameController
virtual para mapping/poll/hot-unplug. Essa suíte native/hardware-facing não pertence ao
gate hermético M12 e só deve rodar intencionalmente. Validação física de input/hotplug
no aparelho continua sendo um gate posterior, nunca uma consequência do teste host.

Um gate estático (`nxinput-static-gate`) complementa a bateria: ele falha se o fonte
de `nxinput` citar nome de CFW, device, firmware ou VID/PID, reforçando que nenhum
fallback por nome de aparelho entra na biblioteca.

## O que continua específico do jogo

`nxinput` resolve aquisição, identidade e normalização. A ponte final não pode ser
universalizada sem conhecer a engine: assinatura JNI, ordem de callbacks, keycodes,
estrutura interna, tela touch-only e o momento de anunciar conexão continuam sendo
confirmados no binário de cada jogo. Essa separação evita que um workaround de um port
altere o fluxo nativo de todos os outros.
