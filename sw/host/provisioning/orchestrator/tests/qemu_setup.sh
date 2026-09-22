#!/bin/bash
# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0

qemu_tmpdir=$(mktemp -d /tmp/ot-qemu-XXXXXX)

mutable_otp="$qemu_tmpdir/otp_img.mut.raw"
if [[ -n "$QEMU_OTP" ]]; then
  cp -f "$QEMU_OTP" "$mutable_otp" && chmod +w "$mutable_otp"
  export QEMU_OTP="$mutable_otp"
fi

mutable_flash="$qemu_tmpdir/flash_img.mut.bin"
if [[ -n "$QEMU_FLASH" ]]; then
  cp -f "$QEMU_FLASH" "$mutable_flash" && chmod +w "$mutable_flash"
  export QEMU_FLASH="$mutable_flash"
fi

export QEMU_SPIFLASH="$qemu_tmpdir/spiflash.bin"
dd if=/dev/zero "of=$QEMU_SPIFLASH" bs=1M count=32 status=none
chmod +w "$QEMU_SPIFLASH"

export QEMU_ICOUNT="${QEMU_ICOUNT:-6}"
export QEMU_LOG="$qemu_tmpdir/qemu.log"
export QEMU_PIDFILE="$qemu_tmpdir/qemu.pid"
export QEMU_MONITOR="$qemu_tmpdir/qemu-monitor"
export QEMU_GPIO="$qemu_tmpdir/qemu-gpio.sock"
export QEMU_RV_DM_JTAG="$qemu_tmpdir/qemu-jtag.sock"
export QEMU_LC_JTAG="$qemu_tmpdir/qemu-jtag-lc-ctrl.sock"

qemu_pid=""
cat_pid=""

cleanup_qemu() {
  local ret=$?
  set +e
  if [[ -f "$QEMU_PIDFILE" ]]; then
    qemu_pid=$(cat "$QEMU_PIDFILE")
  fi
  if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
    echo "Stopping QEMU: $qemu_pid"
    kill "$qemu_pid" 2>/dev/null
    for _ in {1..20}; do
      if ! kill -0 "$qemu_pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$qemu_pid" 2>/dev/null; then
      echo "Killing QEMU: $qemu_pid"
      kill -KILL "$qemu_pid" 2>/dev/null
    fi
  fi
  if [[ -n "$cat_pid" ]] && kill -0 "$cat_pid" 2>/dev/null; then
    kill "$cat_pid" 2>/dev/null || true
  fi
  rm -rf "$qemu_tmpdir"
  exit "$ret"
}
trap cleanup_qemu EXIT

mkfifo "$QEMU_LOG"
cat "$QEMU_LOG" &
cat_pid=$!
"$QEMU_START"
