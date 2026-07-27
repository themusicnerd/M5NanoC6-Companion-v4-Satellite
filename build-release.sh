#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")" && pwd)"
build_dir="$project_dir/build/m5stack.esp32.m5stack_nano_c6"
release_dir="$project_dir/release"
sketch="M5NanoC6-Companion-v4-Satellite.ino"
version="0.1.4"

mkdir -p "$build_dir" "$release_dir"
arduino-cli compile \
  --fqbn m5stack:esp32:m5stack_nano_c6 \
  --board-options CDCOnBoot=cdc,PartitionScheme=min_spiffs,EraseFlash=none \
  --build-path "$build_dir" \
  "$project_dir"

cp "$build_dir/$sketch.bin" \
  "$release_dir/M5NanoC6-Companion-v4-Satellite-v${version}-upgrade.bin"
cp "$build_dir/$sketch.merged.bin" \
  "$release_dir/M5NanoC6-Companion-v4-Satellite-v${version}-factory.bin"
(cd "$release_dir" && sha256sum \
  "./M5NanoC6-Companion-v4-Satellite-v${version}-factory.bin" \
  "./M5NanoC6-Companion-v4-Satellite-v${version}-upgrade.bin" > "SHA256SUMS-v${version}")
