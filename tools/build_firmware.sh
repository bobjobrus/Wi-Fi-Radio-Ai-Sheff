#!/bin/bash
# Сборка прошивки рации (arduino-cli + ядро esp32 3.3.12, плата «ESP32S3 Dev Module»).
#   tools/build_firmware.sh
# Итог в firmware/dist/:
#   walkie-full-vN.bin  — первая прошивка целиком, с адреса 0x0 (esptool или веб-прошивальщик в браузере)
#   walkie.bin + walkie.json — для моста: рации обновятся сами «по воздуху»
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
URL=https://espressif.github.io/arduino-esp32/package_esp32_index.json
FQBN="esp32:esp32:esp32s3:FlashSize=4M,PartitionScheme=min_spiffs,PSRAM=disabled,USBMode=hwcdc,CDCOnBoot=cdc"
OUT="$ROOT/firmware/build"
DIST="$ROOT/firmware/dist"
mkdir -p "$OUT" "$DIST"
if ! arduino-cli core list | grep -q "esp32:esp32 *3.3.12"; then
  arduino-cli core install esp32:esp32@3.3.12 --additional-urls "$URL"
fi
# ctags из набора Arduino есть только под Intel, а на этом Mac нет Rosetta. ctags нужен Arduino, чтобы
# дописать объявления функций в .ino; у нас все функции walkie.ino стоят ДО использования — пустышки хватает.
# -ffile-prefix-map: в прошивку попадают пути файлов ядра (assert) — без домашней папки того, кто собирал
arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT" --warnings default \
  --build-property "tools.ctags.pattern=/usr/bin/true" \
  --build-property "compiler.c.extra_flags=-ffile-prefix-map=$HOME=~" \
  --build-property "compiler.cpp.extra_flags=-ffile-prefix-map=$HOME=~" "$ROOT/firmware/walkie"
VER=$(awk '/^#define FW_VERSION/ {print $3}' "$ROOT/firmware/walkie/config.h")
cp "$OUT/walkie.ino.bin" "$DIST/walkie.bin"
printf '{"version": %s}\n' "$VER" > "$DIST/walkie.json"
cp "$OUT/walkie.ino.merged.bin" "$DIST/walkie-full-v$VER.bin"
ls -l "$DIST"
