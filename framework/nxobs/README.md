# NX observability and support bundles

`nx-support-bundle.py` correlates an `nxbootstrap` runtime log with the separate
NXExtract log under one bounded support run ID. It never copies either raw log.
Instead it emits a versioned JSONL event stream, a sanitized report, a short
human summary and a SHA-256 manifest.

The live runtime remains the authority: explicit `NXEVENT` records retain their
monotonic timestamps and durations. Legacy/plain markers are classified in
their observed order and keep timing fields `null`; the tool does not invent
durations. `NXCOMPAT_REPORT` data is reduced to finite capability IDs, states,
reason codes and bounded receipts. Device names, paths, addresses, hostnames,
credentials and save data never enter the bundle.

Inputs must be regular non-symlink files no larger than 8 MiB, each line is
bounded to 64 KiB, and at most 2,048 events are emitted. The destination must be
a new absolute path under a real directory. All files are written exclusively
to a private temporary directory and renamed only after the manifest exists;
failure leaves no partial bundle. Distinct outputs are independent and an
existing destination is never overwritten.

Example (internal raw paths deliberately omitted):

```sh
python3 -B framework/nxobs/nx-support-bundle.py \
  --runtime-log RUNTIME_LOG --extractor-log NXEXTRACT_LOG \
  --artifact chrono.zip --stack-id arkos-rk3326-kmsdrm \
  --firmware-context arkos --output /absolute/new/support-bundle
```

Raw logs remain internal. Only a sanitized bundle or its manifest hash may be
attached to a public issue.
