#!/bin/sh
# Replay captured ADC banks through receiver.c from a git revision (default: working tree).
# Usage: replay.sh [-r REV] CAPTURE.bin...
# LOST is a valid replay result; malformed/truncated captures fail the command.
set -eu
TOOLS=$(cd "$(dirname "$0")" && pwd)
ROOT=$(git -C "$TOOLS" rev-parse --show-toplevel)
RFID=$ROOT/firmware/application/src/rfid
REV=
if [ "${1:-}" = -r ]; then
  if [ "$#" -lt 3 ]; then
    echo 'usage: replay.sh [-r REV] CAPTURE.bin...' >&2
    exit 1
  fi
  REV=$2
  shift 2
fi
if [ "$#" -eq 0 ]; then
  echo 'usage: replay.sh [-r REV] CAPTURE.bin...' >&2
  exit 1
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/st25tb"
for f in receiver.c receiver.h model.h; do
  if [ -n "$REV" ]; then
    git -C "$ROOT" show "$REV:firmware/application/src/rfid/st25tb/$f" > "$WORK/st25tb/$f"
  else
    cp "$RFID/st25tb/$f" "$WORK/st25tb/$f"
  fi
done
# Earlier receiver revisions predate the generated repair class.
if [ -n "$REV" ]; then
  if git -C "$ROOT" cat-file -e "$REV:firmware/application/src/rfid/st25tb/repair_data.h" 2>/dev/null; then
    git -C "$ROOT" show "$REV:firmware/application/src/rfid/st25tb/repair_data.h" > "$WORK/st25tb/repair_data.h"
  fi
elif [ -f "$RFID/st25tb/repair_data.h" ]; then
  cp "$RFID/st25tb/repair_data.h" "$WORK/st25tb/repair_data.h"
fi
cc -std=c11 -O2 -Wall -Wextra -Werror -I "$WORK" -I "$RFID" "$TOOLS/replay.c" "$WORK/st25tb/receiver.c" \
  "$RFID/iso15693/frame.c" -o "$WORK/replay"
"$WORK/replay" "$@"
