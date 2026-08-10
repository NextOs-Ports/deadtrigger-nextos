# NXExtract milestone audit

The canonical NXExtract source remains in
`suportando_outros_devices/extrator-universal`. This directory contains the
framework-level evidence ledger for milestone M07; it does not duplicate or
vendor the extractor.

`m07-audit-v1.json` maps every M07 requirement to an implementation token and
a regression/gate token. `tests/test_m07_audit.py` verifies the complete ordered
set, source paths, exact 1.2.6 version, 56 synthetic cases and the unchanged
low-glibc UI hash. The audit explicitly does not claim a physical 1.2.6 device
run.
