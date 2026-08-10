# NXRelease

NXRelease é o gate host-side para um ZIP público/multi-device do PortMaster. Ele
recebe um manifesto explícito, copia somente os arquivos declarados para um
stage novo, audita o conteúdo, produz um ZIP determinístico e então abre o ZIP
gerado novamente e repete a verificação. Ele não executa jogo, APK ou loader.

O teto público de GLIBC está gravado no código como `2.30`. O manifesto ou a
linha de comando podem torná-lo mais estrito (por exemplo, `2.17`), nunca mais
permissivo. Todos os ELFs do pacote precisam ser classificados; um ELF Linux
feito pelo projeto ou fornecido por terceiro passa pelo mesmo gate de símbolos
versionados. ELF Android original/BYO, APK, OBB e dex nunca entram no ZIP
público: ficam nos dados fornecidos e extraídos pelo próprio dono.

## Contrato do pacote

O manifesto JSON segue [schema/nxrelease-v2.schema.json](schema/nxrelease-v2.schema.json).
O schema v2 é deliberadamente incompatível com o v1: o v1 não fechava
`DT_NEEDED`, não pinava o helper/receita completos do NXExtract e permitia uma
cadeia de launcher apenas lexical. O gate rejeita v1 em vez de presumir
defaults inseguros.
Os campos essenciais são:

- `package.profile`: sempre `universal-portmaster`;
- `package.launcher`: um único `.sh` na raiz do ZIP, modo `0755`;
- `package.launcher_chain`: exatamente o launcher visível e a biblioteca
  `<port>/nxbootstrap-<versão>.sh` escolhida pelo contrato; a cópia de
  compatibilidade `<port>/nxbootstrap.sh`, o receipt `nxdeployment.json`, os
  scripts e o `nxport.json` são pinados, e o gate valida bytes, receipt canônico
  completo, identidade de implantação e call graph essencial. Contratos anteriores a
  `nxbootstrap 0.5.1` são rejeitados por não terem essa fronteira de deployment;
- `package.launcher_contract`: versão do nxbootstrap e pin de
  `<port>/nxport.json`;
- `package.port_dir`: a pasta do port na raiz;
- `release.source_date_epoch`: timestamp fixo entre 1980 e 2107;
- `release.max_glibc`: opcional, padrão `2.30`;
- `nxextract`: versão e pins do layout canônico inteiro: `extractor.json`,
  `nxextract.py`, `run-extractor.sh` e `nxextract-runtime-env.sh`;
- `dependencies`: provedor explícito para cada chave
  `(namespace, architecture, DT_NEEDED)`;
- `portmaster_metadata`: pins opcionais de `port.json`, `gameinfo.xml` e imagens;
- `files`: fontes e destinos, sem glob implícito e sem caminhos absolutos.

O `nxport.json` pinado precisa ser a saída canônica do schema v2. O gate confere o
objeto NXExtract `1.2.6`, paths privados, capabilities presentes exatamente no
registry finito `framework/nxcompat/capabilities-v1.json`, quirks, relatório e cada
assignment correspondente no launcher visível. Capabilities apenas sintaticamente
válidas não são aceitas; quirks ainda são validados por namespace e sintaxe e devem
permanecer específicos do adapter/jogo até existir um registry finito. Entrada
nxport v1 deve passar primeiro pelo
gerador; o release não completa defaults legados por conta própria.

Uma entrada de diretório é aceita somente para `payload`. Scripts são sempre
arquivos `.sh` individuais e auditáveis. Se houver
um ELF dentro dela, o build falha como “unclassified”; isto é intencional. Cada
ELF deve ter uma entrada de arquivo própria, SHA-256, `DT_NEEDED` exato e
`DT_SONAME` esperado (string ou `null`) com um destes tipos:

- `project-linux`: ELF construído pelo port;
- `third-party-linux`: runtime Linux redistribuído, com proveniência.

Todo ELF precisa ser realmente carregável: `ET_EXEC`/`ET_DYN`, little-endian,
ao menos um `PT_LOAD`, classe coerente com a ABI e, em ARM Linux, EABI5
hard-float. `PT_INTERP` Linux, quando presente, é exatamente
`/lib/ld-linux-aarch64.so.1` ou `/lib/ld-linux-armhf.so.3`. RPATH/RUNPATH é
proibido no perfil universal. Falha em qualquer fase relevante de `readelf`
encerra o gate.

Cada `DT_NEEDED` deve aparecer uma única vez em `dependencies`, separado entre
`linux` e `android` e por ABI. Os provedores possíveis são `package`,
`glibc-base`, `firmware`, `portmaster` e `nxloader-import-registry`. Um provider
`package` aponta para o ELF com aquele `DT_SONAME`; providers duplicados ou
SONAMEs não portáteis são rejeitados. `libstdc++.so.6` e `libgcc_s.so.1` não são
implicitamente parte de `glibc-base`: precisam de contrato real de firmware,
PortMaster ou pacote.

Os dois tipos Linux exigem `architecture`, `provenance` e
`build_profile: "universal-low-glibc"`. Assim uma variante construída contra a
glibc atual não pode ser misturada silenciosamente no ZIP universal, inclusive
quando for estática e não expuser símbolos `GLIBC_*`.

Fonte e licença são obrigatórias no perfil público: o kind `license-notice`
registra um arquivo `LICENSE`/`NOTICE`/
`COPYING` pinado por SHA-256 (modo `0644`), e `package.license` descreve
`spdx_id`, `source_url` da fonte pública e o `file` amarrado. O gate recusa
URLs/fontes que contenham path pessoal ou IP literal e rejeita o manifesto se o
mapa de licença estiver ausente.

O launcher de raiz contém a configuração declarativa, valida o receipt estático
e carrega diretamente `<id>/nxbootstrap-<versão>.sh`. NXRelease confere o
contrato gerado launcher → bootstrap versionado, exige que a cópia
`nxbootstrap.sh` seja byte-idêntica, valida `nxdeployment.json`, o config pinado,
o destino de `exec`/`source` e as funções efetivamente alcançadas que integram
`control.txt`, `get_controls`, `pm_platform_helper` e `pm_finish`. Todos os
arquivos classificados como `script` passam por sintaxe e auditoria; inclusive
um background escondido como `cmd & ;;` é detectado. Drivers SDL/OpenAL fixados,
processos soltos em background, `setsid`/`nohup` e gerenciamento direto do
frontend são bloqueados. As únicas exceções são `adaptive-driver` (retry depois
de uma falha real) e `supervised-child` (PID/trap/wait); cada exceção precisa
nomear o script exato e trazer uma justificativa concreta no manifesto.

`portmaster_metadata` é opcional. Quando declarar `port.json`, `gameinfo.xml` ou
imagens, todos recebem pin SHA-256. O gate valida JSON/XML, itens do ZIP,
launcher, arquiteturas, `min_glibc`, referências de imagem e magic bytes de
PNG/JPEG/WebP. Os tipos correspondentes em `files` são
`portmaster-metadata` e `portmaster-image`.

## Exemplo mínimo de manifesto

```json
{
  "schema_version": 2,
  "source_root": ".",
  "package": {
    "id": "meujogo",
    "version": "1.0.0",
    "profile": "universal-portmaster",
    "launcher": "Meu Jogo.sh",
    "launcher_chain": [
      "Meu Jogo.sh",
      "meujogo/nxbootstrap-0.5.1.sh"
    ],
    "launcher_contract": {
      "generator": "nxbootstrap",
      "version": "0.5.1",
      "config_path": "meujogo/nxport.json",
      "config_sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    "port_dir": "meujogo",
    "license": {
      "spdx_id": "GPL-3.0-only",
      "source_url": "https://example.org/meujogo-source",
      "file": "meujogo/LICENSE"
    }
  },
  "release": {
    "source_date_epoch": 1785542400,
    "max_glibc": "2.30",
    "compression": "deflated"
  },
  "nxextract": {
    "path": "meujogo/nxextract/nxextract.py",
    "version": "1.2.6",
    "minimum_version": "1.2.2",
    "sha256": "COLOQUE_AQUI_O_SHA256_REAL",
    "runner_path": "meujogo/nxextract/run-extractor.sh",
    "runner_sha256": "COLOQUE_AQUI_O_SHA256_REAL",
    "runtime_env_path": "meujogo/nxextract/nxextract-runtime-env.sh",
    "runtime_env_sha256": "COLOQUE_AQUI_O_SHA256_REAL",
    "recipe_path": "meujogo/extractor.json",
    "recipe_sha256": "COLOQUE_AQUI_O_SHA256_REAL"
  },
  "dependencies": [
    {
      "namespace": "linux",
      "architecture": "aarch64",
      "soname": "libSDL2-2.0.so.0",
      "provider": "portmaster"
    },
    {
      "namespace": "linux",
      "architecture": "aarch64",
      "soname": "libc.so.6",
      "provider": "glibc-base"
    }
  ],
  "files": [
    {
      "source": "Meu Jogo.sh",
      "target": "Meu Jogo.sh",
      "kind": "launcher",
      "mode": "0755",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "nxbootstrap.sh",
      "target": "meujogo/nxbootstrap.sh",
      "kind": "script",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "nxbootstrap-0.5.1.sh",
      "target": "meujogo/nxbootstrap-0.5.1.sh",
      "kind": "script",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "nxdeployment.json",
      "target": "meujogo/nxdeployment.json",
      "kind": "payload",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "nxport.json",
      "target": "meujogo/nxport.json",
      "kind": "nxbootstrap-config",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "LICENSE",
      "target": "meujogo/LICENSE",
      "kind": "license-notice",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL"
    },
    {
      "source": "build/loader-aarch64",
      "target": "meujogo/bin/aarch64/loader",
      "kind": "project-linux",
      "mode": "0755",
      "architecture": "aarch64",
      "build_profile": "universal-low-glibc",
      "provenance": "build-universal-aarch64.sh em Debian Buster",
      "sha256": "COLOQUE_AQUI_O_SHA256_REAL",
      "needed": [
        "libSDL2-2.0.so.0",
        "libc.so.6"
      ],
      "soname": null
    },
    {
      "source": "vendor/nxextract.py",
      "target": "meujogo/nxextract/nxextract.py",
      "kind": "nxextract",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "vendor/run-extractor.sh",
      "target": "meujogo/nxextract/run-extractor.sh",
      "kind": "nxextract-runner",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "vendor/nxextract-runtime-env.sh",
      "target": "meujogo/nxextract/nxextract-runtime-env.sh",
      "kind": "nxextract-runtime-env",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "extractor.json",
      "target": "meujogo/extractor.json",
      "kind": "nxextract-recipe",
      "mode": "0644",
      "sha256": "COLOQUE_AQUI_O_MESMO_SHA256"
    },
    {
      "source": "payload",
      "target": "meujogo/assets",
      "kind": "payload"
    }
  ]
}
```

Os placeholders precisam ser substituídos pelo resultado de `sha256sum`. O
validador interno é a autoridade; o JSON Schema serve para autocomplete e
validação inicial no editor.

## Uso

Requisitos: host Linux, Python 3.8 ou mais novo, GNU `readelf`, `bash` e
`/bin/sh`. `stage`/`bundle` exigem `renameat2(RENAME_NOREPLACE)` no filesystem;
o `build` direto também exige hard links no diretório de saída. O gate falha de
forma fechada se o host não puder garantir no-overwrite.

```sh
python3 framework/nxrelease/nxrelease.py validate \
  --manifest ports/meujogo/nxrelease.json

python3 framework/nxrelease/nxrelease.py build \
  --manifest ports/meujogo/nxrelease.json \
  --stage /caminho/local/meujogo-stage \
  --output /caminho/local/meujogo.zip

# Publicação conjunta/crash-atômica recomendada:
python3 framework/nxrelease/nxrelease.py bundle \
  --manifest ports/meujogo/nxrelease.json \
  --stage /caminho/local/meujogo-stage-publicacao \
  --destination /caminho/publico/meujogo-1.0.0 \
  --archive-name meujogo.zip

python3 framework/nxrelease/nxrelease.py verify \
  --archive /caminho/local/meujogo.zip \
  --sha256-file /caminho/local/meujogo.zip.sha256
```

`stage`, `output` e `destination` nunca são sobrescritos, inclusive se outro processo criar o
destino entre a validação e a publicação. O ZIP e seu `.sha256` são preparados
e verificados antes da publicação. O comando `build` usa links no-replace, lock
exclusivo e rollback por inode para a dupla no mesmo diretório. Como POSIX não
oferece uma transação de duas entradas, `bundle` é o caminho público mais forte:
monta ZIP + `.sha256` em diretório oculto e faz uma única renomeação
`RENAME_NOREPLACE` do diretório, dando visibilidade conjunta mesmo diante de
queda do processo. Use destinos novos para cada release.
O comando `build` só termina depois de verificar o stage, criar o ZIP, reabri-lo,
conferir CRC, ordem, timestamps, modos, inventário, `MANIFEST.sha256`, NXExtract,
todos os ELFs e o SHA-256 externo. Também existem `stage` e `verify-stage` para
diagnóstico separado. Cada arquivo é hasheado novamente após a cópia e a fonte
é conferida outra vez, fechando a janela entre `validate` e `stage`.

O stage contém três arquivos gerados dentro de
`<port_dir>/.nxrelease/`, sem colisão na raiz compartilhada do PortMaster:

- `<port_dir>/.nxrelease/NXRELEASE-METADATA.json`: inventário, modos, contrato
  de dependências, pins nxbootstrap/NXExtract e relatório de
  todos os ELFs (classe, máquina, flags ABI, `PT_INTERP`, `DT_NEEDED`,
  `DT_SONAME`, GLIBC máxima, proveniência e hash);
- `<port_dir>/.nxrelease/SBOM.cdx.json`: CycloneDX 1.5 determinístico
  (serial e timestamp derivados do `package.id` e do `source_date_epoch`),
  projetado a partir do inventário auditado; cada componente de arquivo traz
  SHA-256, kind, modo e, para ELFs, arquitetura/`glibc_max`/`interpreter`/
  proveniência. `verify` reconfere a cobertura e os hashes contra o inventário;
- `<port_dir>/.nxrelease/MANIFEST.sha256`: SHA-256 de cada arquivo do pacote,
  inclusive os metadados e o SBOM, excluindo apenas ele próprio para evitar
  recursão.

`port.json.items` aceita a convenção PortMaster de exatamente uma `/` final em
diretórios (`"meujogo/"`). O gate remove essa barra para comparar, prova que o
membro é de fato um diretório e rejeita `//` ou uma `/` aplicada a arquivo.

Não publique apenas porque este gate passou. O ZIP exato ainda precisa ser
instalado de forma virgem e testado em aparelhos reais nas fases definidas pelo
projeto; qualquer mudança posterior gera ZIP/hash novos e reinicia esse teste.

## Fechamento M18

`m18-closure-v1.json` liga M18-001..024 a arquivo/linha, garantia, limite de
escopo e teste. O validador puro confirma que os 24 itens estão fechados, que
Android/BYO não voltou à allowlist pública, que licença é obrigatória e que o
corpus adversarial contém traversal, symlink, colisão Unicode, dados privados,
artefatos Android, tamper, corrida e no-overwrite:

```sh
python3 -B framework/nxrelease/tests/test_m18_closure.py
bash framework/nxrelease/tests/test_nxrelease.sh
```

`closed` aqui significa gate host-side completo. Não significa que o agente
jogou o port nem substitui a instalação virgem e a aceitação humana do ZIP
exato, que continuam em milestones posteriores.

## Testes

Os testes usam somente fixtures locais e não acessam rede. Eles constroem ELFs
reais de teste e portanto requerem `aarch64-linux-gnu-gcc`, `clang` e `ld.lld`
além dos requisitos normais do gate:

```sh
bash framework/nxrelease/tests/test_nxrelease.sh
```
