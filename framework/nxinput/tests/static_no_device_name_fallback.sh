#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Static gate: nxinput must never select behavior by device name, CFW label,
# firmware string or a hard-coded VID/PID. Those are diagnostic facts owned by
# the PortMaster/firmware mapping and by nxcompat, never by this module.
#
# Arguments:
#   $1 - nxinput source root (defaults to the script's own directory/.. )
#
# Exits 0 when the scanned sources are clean, 1 on the first forbidden token.

set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "$#" -ge 1 ] && [ -n "${1:-}" ]; then
  root="$1"
fi

scan_dir="$root/src"
header_dir="$root/include"
if [ ! -d "$scan_dir" ] || [ ! -d "$header_dir" ]; then
  echo "static-no-device-name-fallback: source tree not found under '$root'" >&2
  exit 1
fi

# Lower-cased substrings that would betray a name/firmware/VID-PID fallback.
denylist=(
  'nextos' 'arkos' 'rocknix' 'knulli' 'batocera' 'muos' 'miyoo' 'trimui'
  'retrodeck' 'emuelec' 'darkos' 'jelos' 'panfrost' 'odroid'
  'gameforce' 'anbernic' 'powkiddy' 'mali-450' 'x5m'
  '0810:0001' '054c:' '0b05:' '0e6f:' '28de:'
)

status=0
for token in "${denylist[@]}"; do
  # -I so grep treats the pattern as fixed; case-insensitive.
  if grep -rIn --include='*.c' --include='*.h' -i -F -- "$token" \
       "$scan_dir" "$header_dir"; then
    echo "static-no-device-name-fallback: forbidden device/firmware token '$token' present" >&2
    status=1
  fi
done

if [ "$status" -eq 0 ]; then
  echo "static-no-device-name-fallback: clean"
fi
exit "$status"
