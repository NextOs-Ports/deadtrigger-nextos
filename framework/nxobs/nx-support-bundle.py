#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Create a bounded, sanitized support bundle from runtime logs.

The source logs remain private.  The published bundle contains only finite
event classes, stable reason codes, selected capability facts and SHA-256
fingerprints that let a maintainer correlate the original evidence later.
"""

from __future__ import print_function

import argparse
import datetime
import errno
import hashlib
import io
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import sys
import zipfile


SCHEMA_VERSION = 1
MAX_INPUT_BYTES = 8 * 1024 * 1024
MAX_LINE_BYTES = 65536
MAX_EVENTS = 2048
ID = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,95}$")
PRIVATE = re.compile(
    r"(?:\b(?:10|127|169\.254|192\.168|172\.(?:1[6-9]|2\d|3[01]))"
    r"(?:\.\d{1,3}){2,3}\b|(?:/[A-Za-z0-9._-]+){2,}|"
    r"\b(?:root|admin|user)@|(?:password|passwd|credential|token|secret)\s*[=:])",
    re.IGNORECASE)
HEX64 = re.compile(r"^[0-9a-f]{64}$")
SAFE_TEXT = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._:+/()@-]{0,95}$")
EVENT_SOURCES = {
    "bootstrap", "extractor", "loader", "graphics", "audio", "input",
    "lifecycle", "diagnostic",
}
EVENT_STATUSES = {"begin", "ok", "failed", "observed", "skipped"}


class BundleError(Exception):
    pass


def sha256_bytes(value):
    return hashlib.sha256(value).hexdigest()


def file_bytes(path, maximum=MAX_INPUT_BYTES):
    path = Path(path)
    try:
        info = path.lstat()
    except OSError as error:
        raise BundleError("input unavailable") from error
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise BundleError("input must be a regular non-symlink file")
    if info.st_size > maximum:
        raise BundleError("input exceeds the bounded size")
    with path.open("rb") as stream:
        value = stream.read(maximum + 1)
    if len(value) > maximum:
        raise BundleError("input grew beyond the bounded size")
    return value


def bounded_lines(value):
    lines = value.splitlines()
    for line in lines:
        if len(line) > MAX_LINE_BYTES:
            raise BundleError("log line exceeds the bounded size")
    return [line.decode("utf-8", "replace") for line in lines]


def safe_text(value, fallback="unknown"):
    if not isinstance(value, str) or not SAFE_TEXT.fullmatch(value):
        return fallback
    if PRIVATE.search(value):
        return fallback
    return value


def append_event(events, run_id, source, phase, status, reason_code,
                 details=None, source_time=None, monotonic_ns=None,
                 duration_ns=None):
    if len(events) >= MAX_EVENTS:
        raise BundleError("event count exceeds the bounded maximum")
    if source not in EVENT_SOURCES or status not in EVENT_STATUSES:
        raise BundleError("event enum is outside the finite schema")
    if not isinstance(reason_code, int) or reason_code < 0 or reason_code > 9999:
        raise BundleError("event reason code is invalid")
    event = {
        "schema": "nx-event-v1",
        "schema_version": 1,
        "sequence": len(events) + 1,
        "run_id": run_id,
        "source": source,
        "phase": safe_text(phase),
        "status": status,
        "reason_code": reason_code,
        "source_time": source_time,
        "monotonic_ns": monotonic_ns,
        "duration_ns": duration_ns,
        "details": details or {},
    }
    events.append(event)


def parse_explicit_event(line, events, run_id):
    marker = "NXEVENT "
    if not line.startswith(marker):
        return False
    try:
        item = json.loads(line[len(marker):])
    except (TypeError, ValueError):
        raise BundleError("malformed NXEVENT record")
    if item.get("schema") != "nx-event-v1":
        raise BundleError("unknown NXEVENT schema")
    monotonic_ns = item.get("monotonic_ns")
    duration_ns = item.get("duration_ns")
    for name, value in (("monotonic_ns", monotonic_ns),
                        ("duration_ns", duration_ns)):
        if value is not None and (not isinstance(value, int) or value < 0):
            raise BundleError("invalid %s" % name)
    raw_details = item.get("details", {})
    if not isinstance(raw_details, dict) or len(raw_details) > 16:
        raise BundleError("NXEVENT details are not bounded")
    details = {}
    for key, value in raw_details.items():
        safe_key = safe_text(key)
        if safe_key == "unknown":
            continue
        if isinstance(value, bool) or isinstance(value, int):
            details[safe_key] = value
        elif isinstance(value, str):
            details[safe_key] = safe_text(value)
    append_event(events, run_id, item.get("source"), item.get("phase"),
                 item.get("status"), item.get("reason_code"), details,
                 safe_text(item.get("source_time"), None), monotonic_ns,
                 duration_ns)
    return True


def receipt_summary(receipt):
    if not isinstance(receipt, dict):
        return None
    result = {}
    for key in ("source", "generation", "proof_flags", "connected_count",
                "frequency", "channels", "samples", "backend", "gl_vendor",
                "gl_renderer", "gl_version", "egl_vendor", "egl_version"):
        value = receipt.get(key)
        if isinstance(value, bool) or isinstance(value, int):
            result[key] = value
        elif isinstance(value, str):
            result[key] = safe_text(value)
    for key in ("window", "drawable", "gles", "rgba"):
        value = receipt.get(key)
        if (isinstance(value, list) and len(value) <= 4 and
                all(isinstance(item, int) and 0 <= item <= 32768
                    for item in value)):
            result[key] = value
    return result


def parse_nxcompat(line, events, run_id):
    marker = "NXCOMPAT_REPORT "
    offset = line.find(marker)
    if offset < 0:
        return False
    try:
        encoded = line[offset + len(marker):].lstrip()
        report, _unused_offset = json.JSONDecoder().raw_decode(encoded)
    except (TypeError, ValueError):
        append_event(events, run_id, "diagnostic", "capability-report",
                     "failed", 1950)
        return True
    phase = safe_text(report.get("phase"), "capabilities")
    details = {"report_reason_code": int(report.get("report_reason_code", 0))}
    host = report.get("host", {})
    if isinstance(host, dict):
        details["host"] = {
            key: safe_text(host.get(key))
            for key in ("process_arch", "kernel_arch", "libc", "memory_class",
                        "filesystem_class")
        }
    decisions = []
    for item in report.get("capabilities", []):
        if not isinstance(item, dict) or len(decisions) >= 64:
            continue
        identifier = safe_text(item.get("id"))
        state = safe_text(item.get("state"))
        reason = item.get("reason_code")
        if identifier != "unknown" and state != "unknown" and isinstance(reason, int):
            decisions.append({"id": identifier, "state": state,
                              "reason_code": reason})
    details["capabilities"] = decisions
    receipts = {}
    for name in ("graphics", "audio", "input"):
        value = receipt_summary(report.get("receipts", {}).get(name))
        if value:
            receipts[name] = value
    details["receipts"] = receipts
    append_event(events, run_id, "bootstrap", phase, "observed",
                 int(report.get("report_reason_code", 0)), details)
    return True


def parse_runtime(lines, events, run_id):
    for line in lines:
        if parse_explicit_event(line, events, run_id):
            continue
        if parse_nxcompat(line, events, run_id):
            if "game exited with status 0" in line:
                append_event(events, run_id, "lifecycle", "shutdown", "ok", 1802)
            continue
        lower = line.lower()
        if "run_start_utc=" in line:
            append_event(events, run_id, "bootstrap", "run", "begin", 1000)
        elif "running NXExtract" in line:
            append_event(events, run_id, "extractor", "validation", "begin", 1400)
        elif line.startswith("NXLOADER[") and " loaded " in line:
            append_event(events, run_id, "loader", "mapping", "ok", 1500)
        elif line.startswith("NXLOADER prepared"):
            append_event(events, run_id, "loader", "relocations", "ok", 1501)
        elif "constructors -> JNI_OnLoad" in line:
            append_event(events, run_id, "loader", "initializers-jni", "begin", 1502)
        elif "NXLOADER game READY JNI=" in line:
            append_event(events, run_id, "loader", "jni", "ok", 1503)
        elif line.startswith("NXGL[") or line.startswith("VIDEO "):
            append_event(events, run_id, "graphics", "context", "observed", 1600)
        elif line.startswith("AUDIO:") or "audio opened:" in line:
            append_event(events, run_id, "audio", "output", "ok", 1700)
        elif "Entering main loop" in line:
            append_event(events, run_id, "lifecycle", "loop", "ok", 1800)
        elif "nativeOnPause" in line:
            append_event(events, run_id, "lifecycle", "pause-save", "ok", 1801)
        elif "game exited with status 0" in line:
            append_event(events, run_id, "lifecycle", "shutdown", "ok", 1802)
        elif ("error" in lower or "failed" in lower or "fatal" in lower) and not (
                "errors=0" in lower or "failed=0" in lower):
            append_event(events, run_id, "diagnostic", "runtime", "failed", 1900)


def parse_extractor(lines, events, run_id):
    timestamp = re.compile(r"^\[([0-9]{4}-[0-9]{2}-[0-9]{2} [0-9:]{8})\] (.*)$")
    for line in lines:
        match = timestamp.match(line)
        if not match:
            continue
        source_time, message = match.groups()
        lower = message.lower()
        if "=== nxextract" in lower:
            phase, status, reason = "run", "begin", 1400
        elif "fast validation marker accepted" in lower:
            phase, status, reason = "validation", "ok", 1401
        elif "adopted fully validated" in lower:
            phase, status, reason = "adoption", "ok", 1402
        elif "validated payload committed" in lower:
            phase, status, reason = "commit", "ok", 1403
        elif "installation complete" in lower:
            phase, status, reason = "install", "ok", 1404
        elif "rejected" in lower:
            phase, status, reason = "validation", "observed", 1405
        elif "failed" in lower or "error" in lower:
            phase, status, reason = "run", "failed", 1499
        else:
            continue
        append_event(events, run_id, "extractor", phase, status, reason,
                     source_time=source_time)


def write_file(path, value):
    path = Path(path)
    with path.open("xb") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=True, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def artifact_components(value):
    """Read only finite component hashes from the package's own manifest."""
    wanted = {
        "chrono-universal": "loader-adapter-shims",
        "nxbootstrap.sh": "nxbootstrap",
        "nxextract.py": "nxextract",
        "nxport.json": "manifest",
    }
    result = {}
    try:
        with zipfile.ZipFile(io.BytesIO(value)) as archive:
            names = [name for name in archive.namelist()
                     if name.endswith("/PACKAGE-MANIFEST.sha256")]
            if len(names) != 1:
                return result
            info = archive.getinfo(names[0])
            if info.file_size > 65536:
                return result
            manifest = archive.read(info).decode("ascii")
    except (KeyError, UnicodeError, ValueError, zipfile.BadZipFile):
        return result
    for line in manifest.splitlines():
        fields = line.split("  ", 1)
        if len(fields) != 2 or not HEX64.fullmatch(fields[0]):
            continue
        basename = fields[1].rsplit("/", 1)[-1]
        component = wanted.get(basename)
        if component:
            result[component] = fields[0]
    return result


def build_bundle(runtime_log, extractor_log, output, stack_id,
                 firmware_context, artifact=None, run_id=None,
                 created_utc=None, payload_abi="unknown",
                 rom_root_kind="unknown", writer=write_file):
    if (not ID.fullmatch(stack_id) or not ID.fullmatch(firmware_context) or
            not ID.fullmatch(payload_abi) or not ID.fullmatch(rom_root_kind)):
        raise BundleError("stack/context ID is outside the finite grammar")
    if run_id is None:
        run_id = "support-%s-%s" % (
            datetime.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ"),
            secrets.token_hex(4))
    if not RUN_ID.fullmatch(run_id):
        raise BundleError("run ID is outside the finite grammar")
    if created_utc is None:
        created_utc = datetime.datetime.utcnow().replace(
            microsecond=0).isoformat() + "Z"
    runtime = file_bytes(runtime_log)
    extractor = file_bytes(extractor_log)
    events = []
    parse_extractor(bounded_lines(extractor), events, run_id)
    # NXExtract is an isolated foreground phase before the loader runtime.
    parse_runtime(bounded_lines(runtime), events, run_id)
    if not events:
        raise BundleError("no recognized bounded events")
    artifact_sha = None
    components = {"nxobs": "0.1.0"}
    if artifact is not None:
        artifact_value = file_bytes(artifact, maximum=64 * 1024 * 1024)
        artifact_sha = sha256_bytes(artifact_value)
        components.update(artifact_components(artifact_value))
    timed = sum(1 for item in events if item["monotonic_ns"] is not None)
    categories = sorted({item["source"] for item in events})
    last = events[-1]
    terminal = [item for item in events
                if item["source"] == "lifecycle" and
                item["phase"] == "shutdown" and item["status"] == "ok"]
    last_completed = terminal[-1] if terminal else last
    observed_host = {}
    for event in events:
        host = event.get("details", {}).get("host")
        if isinstance(host, dict):
            observed_host = host
    report = {
        "schema": "nx-support-bundle-v1",
        "schema_version": 1,
        "run_id": run_id,
        "created_utc": created_utc,
        "stack_id": stack_id,
        "firmware_context": firmware_context,
        "payload_abi": payload_abi,
        "rom_root_kind": rom_root_kind,
        "artifact_sha256": artifact_sha,
        "components": components,
        "observed_host": observed_host,
        "path_classes": {
            "home": "port-relative", "save": "port-relative-userdata",
            "cache": "port-relative-cache",
        },
        "library_path_origins": ["private", "portmaster", "firmware"],
        "sources": {
            "runtime": {"sha256": sha256_bytes(runtime), "bytes": len(runtime),
                        "lines": len(runtime.splitlines())},
            "extractor": {"sha256": sha256_bytes(extractor),
                          "bytes": len(extractor),
                          "lines": len(extractor.splitlines())},
        },
        "event_count": len(events),
        "timed_event_count": timed,
        "categories": categories,
        "last_completed": {
            "sequence": last_completed["sequence"],
            "source": last_completed["source"],
            "phase": last_completed["phase"],
            "status": last_completed["status"],
            "reason_code": last_completed["reason_code"],
        },
        "privacy": {
            "raw_logs_included": False,
            "paths_included": False,
            "addresses_included": False,
            "hostnames_included": False,
            "credentials_included": False,
            "save_data_included": False,
        },
        "limits": {"maximum_input_bytes": MAX_INPUT_BYTES,
                   "maximum_line_bytes": MAX_LINE_BYTES,
                   "maximum_events": MAX_EVENTS},
    }
    schema = {
        "schema": "nx-observability-schema-v1", "schema_version": 1,
        "event_schema": "nx-event-v1", "bundle_schema": "nx-support-bundle-v1",
        "maximum_input_bytes": MAX_INPUT_BYTES,
        "maximum_line_bytes": MAX_LINE_BYTES, "maximum_events": MAX_EVENTS,
    }
    events_value = b"".join(json_bytes(item) for item in events)
    summary = (
        "NX support bundle\nrun_id=%s\nstack=%s\nevents=%d\n"
        "last=%s/%s/%s\nraw_logs_included=0\n" %
        (run_id, stack_id, len(events), last_completed["source"],
         last_completed["phase"], last_completed["status"])).encode("ascii")
    files = {
        "events.jsonl": events_value,
        "report.json": json_bytes(report),
        "schema.json": json_bytes(schema),
        "SUMMARY.txt": summary,
    }
    output = Path(output)
    parent = output.parent
    if not output.is_absolute() or output.exists() or output.is_symlink():
        raise BundleError("output must be a new absolute path")
    parent_info = parent.lstat()
    if stat.S_ISLNK(parent_info.st_mode) or not stat.S_ISDIR(parent_info.st_mode):
        raise BundleError("output parent must be a real directory")
    temporary = parent / (".%s.tmp.%d.%s" %
                          (output.name, os.getpid(), secrets.token_hex(4)))
    temporary.mkdir(mode=0o700)
    try:
        for name in sorted(files):
            writer(temporary / name, files[name])
        manifest = b"".join(
            ("%s  %s\n" % (sha256_bytes(files[name]), name)).encode("ascii")
            for name in sorted(files))
        writer(temporary / "MANIFEST.sha256", manifest)
        os.replace(str(temporary), str(output))
    except Exception:
        if temporary.exists():
            shutil.rmtree(str(temporary))
        raise
    return report


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime-log", required=True)
    parser.add_argument("--extractor-log", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--stack-id", required=True)
    parser.add_argument("--firmware-context", required=True)
    parser.add_argument("--artifact")
    parser.add_argument("--run-id")
    parser.add_argument("--created-utc")
    parser.add_argument("--payload-abi", default="unknown")
    parser.add_argument("--rom-root-kind", default="unknown")
    args = parser.parse_args(argv)
    try:
        report = build_bundle(
            args.runtime_log, args.extractor_log, args.output, args.stack_id,
            args.firmware_context, args.artifact, args.run_id, args.created_utc,
            args.payload_abi, args.rom_root_kind)
    except (BundleError, OSError, ValueError) as error:
        print("nx-support-bundle: %s" % error, file=sys.stderr)
        return 1
    print("nx_support_bundle=PASS run_id=%s events=%d raw_logs=0 "
          "addresses=0 credentials=0" %
          (report["run_id"], report["event_count"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
