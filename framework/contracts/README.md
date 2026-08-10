# Contratos compartilhados do framework

[`declarative-v1.json`](declarative-v1.json) é o lock machine-readable das três
camadas e das versões compatíveis. Ele impede que uma conveniência de um port seja
silenciosamente promovida a comportamento global.

## Três camadas

- `pre-main` pertence ao `nxbootstrap`: PortMaster, ABI/`PT_INTERP`, NXExtract,
  bibliotecas declaradas, lock e supervisão do filho direto;
- `runtime-common` pertence a `nxcompat`, `nxgl`, `nxinput`, `nxloader` e
  `nxandroid`: probe, plano process-local, abertura/relatório real, mapping,
  imports explícitos e validação do perfil de lifecycle declarado pelo adapter;
- `adapter` pertence ao port: lifecycle Android/engine, JNI, saves, UI, shutdown e
  quirks específicos com evidência.

O shell não pode receber fluxo nativo da engine, tabela de device, backend forçado ou
administração do frontend. O núcleo comum não pode deduzir capacidade pelo nome do
firmware. O adapter não pode pular `init_array`, `JNI_OnLoad` ou etapas originais.

## Versões

Os componentes `nxcompat`, `nxgl`, `nxinput`, `nxloader` e `nxandroid` usam API
versionada e `struct_size`; compatibilidade precisa ser explícita. `nxandroid`
valida fronteiras e ordem, mas não fornece uma VM/JNI genérica nem inventa
callbacks da engine. `nxgl` 0.2 mantém a API 1 literal e publica a API 2 como
versão corrente, com ownership do stack e callbacks congelado até `close`.
O conjunto gerado pelo `nxbootstrap` é pinado como uma
unidade exata. NXRelease exige seu manifesto v2, e NXExtract `1.2.6` é um
conjunto de quatro conteúdos pinados, não apenas um nome de versão.

O gate compara cada `current_version` com o arquivo `VERSION` real e cada API C com o
header público correspondente.

## `nxport.json` v2

O schema atual está em
[`../nxbootstrap/schema/nxport-v2.schema.json`](../nxbootstrap/schema/nxport-v2.schema.json).
Ele acrescenta:

- objeto `nxextract` com pin exato `1.2.6`;
- `private_library_paths`, separados do firmware/PortMaster;
- `required_capabilities` nos namespaces `host`, `graphics`, `audio` e `input`;
- `enabled_quirks` nos namespaces `adapter`, `engine` e `game`, vazio por padrão;
- `runtime_report` obrigatório em `log` ou `log-and-logo`.

Nenhum nome pode usar `device.*`. Capabilities declaram fatos que o adapter ainda
precisa comprovar; quirks apenas habilitam código específico já implementado e
documentado. Essas listas nunca definem variável SDL/Mesa nem selecionam correção por
modelo.

`home_mode: preserve` é o default. `port` só entra com prova da engine. Save/cache
adicionais continuam responsabilidade contida do adapter; o manifesto genérico não os
redireciona por firmware.

Entrada v1 é aceita somente pelo gerador e atualizada deterministicamente. Toda saída
é v2; release público rejeita v1 não regenerado. Campos desconhecidos — inclusive o
antigo `process_names` — falham fechado.

## Gates

```sh
python3 -B framework/nxbootstrap/tests/test-manifest-contract.py
bash framework/nxbootstrap/tests/run-isolated.sh
bash framework/nxrelease/tests/test_nxrelease.sh
```

O primeiro gate não cria processos de port. O segundo executa lifecycle/gerador
somente num PID namespace privado. O terceiro constrói ELFs sintéticos e prova que o
release compara o manifesto v2 com os assignments reais do launcher visível.
