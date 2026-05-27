#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

exec "$SCRIPT_DIR/build/altair-cpm-mcp" \
  "$SCRIPT_DIR/disks/cpm63k.dsk" \
  "$SCRIPT_DIR/disks/bdsc-v1.60.dsk" \
  "$SCRIPT_DIR/disks/blank.dsk" \
  "$SCRIPT_DIR/../Disks/cpm63k.dsk" \
  "$SCRIPT_DIR/../Disks/bdsc-v1.60.dsk" \
  "$SCRIPT_DIR/../Disks/blank.dsk" \
  "$SCRIPT_DIR/../Apps"
