#!/bin/sh

set -eu

: "${ALTAIR_APPS_ROOT:=/opt/altair/Apps}"
: "${ALTAIR_ENV_FILE:=/opt/altair/runtime/altair_env.txt}"
: "${ALTAIR_DISKS_DIR:=/opt/altair/disks}"
: "${ALTAIR_DRIVE_A:=cpm63k.dsk}"
: "${ALTAIR_DRIVE_B:=bdsc-v1.60.dsk}"
: "${ALTAIR_DRIVE_C:=escape-posix.dsk}"
: "${ALTAIR_DRIVE_D:=blank.dsk}"

drive_a_path=${ALTAIR_DRIVE_A_PATH:-$ALTAIR_DISKS_DIR/$ALTAIR_DRIVE_A}
drive_b_path=${ALTAIR_DRIVE_B_PATH:-$ALTAIR_DISKS_DIR/$ALTAIR_DRIVE_B}
drive_c_path=${ALTAIR_DRIVE_C_PATH:-$ALTAIR_DISKS_DIR/$ALTAIR_DRIVE_C}
drive_d_path=${ALTAIR_DRIVE_D_PATH:-$ALTAIR_DISKS_DIR/$ALTAIR_DRIVE_D}

exec altair-local \
    --apps-root "$ALTAIR_APPS_ROOT" \
    --env-file "$ALTAIR_ENV_FILE" \
    --drive-a "$drive_a_path" \
    --drive-b "$drive_b_path" \
    --drive-c "$drive_c_path" \
    --drive-d "$drive_d_path" \
    "$@"
