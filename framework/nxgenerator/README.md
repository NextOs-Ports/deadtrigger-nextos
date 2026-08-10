# nxgenerator

`nxgenerator` fecha a camada de geração do M19 sem inventar comportamento da engine.
Ele recebe um manifesto de projeto, reutiliza o gerador canônico do `nxbootstrap` e
publica uma árvore nova de forma no-replace.

## Entrada e saída

O schema está em [`schema/nxproject-v1.schema.json`](schema/nxproject-v1.schema.json).
Há exemplos separados para
[`AArch64`](examples/nxproject-aarch64.example.json) e
[`ARMv7/ARMHF`](examples/nxproject-armv7.example.json).

```sh
python3 framework/nxgenerator/nxgenerator.py \
  framework/nxgenerator/examples/nxproject-aarch64.example.json \
  --output /tmp/nxexample-project
```

A saída contém:

```text
NXExample AArch64.sh
nxexample-aarch64/
├── nxport.json
├── nxproject.json
├── extractor.json
├── nxextract/
├── adapter/adapter-contract.json
├── port.json
├── README.md
├── LICENSE
└── GENERATION.json
```

O gerador usa a versão exata de `nxbootstrap` declarada por
[`../nxbootstrap/VERSION`](../nxbootstrap/VERSION). Desde o 0.6.0, o produto tem um
único launcher visível e autocontido: o comportamento pré-main é materializado nele, sem
biblioteca de runtime, `nxdeployment.json` ou `run.sh` secundário. O launcher e o
`nxport.json` formam o conjunto mínimo gerado; os hashes do template e do gerador
canônicos ficam pinados em `GENERATION.json`.

O gerador rejeita antes da publicação launcher com modo inseguro, pin stale, manifesto
divergente ou qualquer artefato aposentado (`nxbootstrap-*.sh`,
`nxbootstrap.sh`, `nxdeployment.json` ou `run.sh`). Quando NXExtract está
ativo, a receita é obrigatória e o conjunto
`nxextract.py`, `run-extractor.sh` e `nxextract-runtime-env.sh` vem integralmente do
NXExtract 1.2.6 canônico. O recibo `GENERATION.json` fixa os sources do gerador,
template e NXExtract, e inventaria com modo e SHA-256 o launcher, o manifesto e os demais
artefatos.

## Adapter deliberadamente vazio

`adapter-contract.json` nasce como `unimplemented_nonrelease`, sem ordem de lifecycle,
JNI, callbacks, offsets, formato de áudio, mapping, save ou ação terminal. O gerador não
adivinha nenhuma dessas decisões. Elas só podem ser preenchidas pelo adapter do jogo com
fonte e teste próprios.

## Documentação pública

O README gerado segue a organização bilíngue do README aprovado do GTA San Andreas:
visão geral, arquitetura, problemas resolvidos, controles, dados, build/run, mapa de
fontes e licenças. Ele também separa baseline de suporte físico, explica quirks estreitos,
marca standalone como desenvolvimento e exige o mesmo ZIP/SHA para qualquer alegação.

O texto inicial é um esqueleto não publicável. Nenhum endereço, hostname, caminho
pessoal, credencial ou log bruto é aceito como evidência pública, e nenhuma linha gerada
declara suporte físico.

## Gate

```sh
python3 -B framework/nxgenerator/tests/test_nxgenerator.py
python3 -B framework/nxgenerator/tests/test_m19_closure.py
```

O primeiro gate gera duas árvores limpas para cada ABI e compara bytes e modos. Também
confere o launcher autocontido atual, rejeita quinze adulterações/artefatos aposentados,
valida pins, metadata PortMaster, NXExtract, documentação, contenção de paths e publicação
no-overwrite. O segundo liga cada requisito M19 à implementação e ao teste.
