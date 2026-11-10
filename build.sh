#!/bin/sh
# Build + flash the Stick S3 port. NimBLE's GAP service needs the PPCP
# characteristic so the Service Changed CCCD lands at handle 0x0B like a
# SoftDevice table (phones that cached a real Plus+ rely on it); those are
# library syscfg values, so they are passed as compiler flags here.
set -e
cd "$(dirname "$0")"
PPCP='-DMYNEWT_VAL_BLE_SVC_GAP_PPCP_MIN_CONN_INTERVAL=6 -DMYNEWT_VAL_BLE_SVC_GAP_PPCP_MAX_CONN_INTERVAL=24 -DMYNEWT_VAL_BLE_SVC_GAP_PPCP_SLAVE_LATENCY=0 -DMYNEWT_VAL_BLE_SVC_GAP_PPCP_SUPERVISION_TMO=500'
arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3:PSRAM=opi --build-path build \
  --build-property "compiler.c.extra_flags=$PPCP" --build-property "compiler.cpp.extra_flags=$PPCP" PlusPlusStick
if [ "$1" = "flash" ]; then
  # --after watchdog-reset: the only reset that boots the app from macOS (DTR/RTS resets park the S3 in download mode)
  esptool --chip esp32s3 --port "${PORT:-/dev/cu.usbmodem21101}" --baud 921600 --after watchdog-reset write-flash -z 0x10000 build/PlusPlusStick.ino.bin
fi
