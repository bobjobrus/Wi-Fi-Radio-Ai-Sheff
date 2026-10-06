#!/bin/bash
# Первая прошивка рации по USB-кабелю с этого Mac.
#   tools/flash.sh              — сам найдёт порт платы
#   tools/flash.sh /dev/cu.xxx  — указать порт
# Дальше рация обновляется сама с моста (или файлом walkie.bin на своей странице).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESPTOOL=$(ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1)
[ -x "$ESPTOOL" ] || { echo "нет esptool: сначала tools/build_firmware.sh (поставит ядро esp32)"; exit 1; }
PORT="${1:-$(ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1 || true)}"
[ -n "$PORT" ] || { echo "плата не найдена: подключите кабелем с данными (не только зарядным)"; exit 1; }
BIN=$(ls -t "$ROOT"/firmware/dist/walkie-full-v*.bin | head -1)
echo "порт $PORT, файл $(basename "$BIN")"
# --after watchdog-reset: 29.09 плату 2 шили через гнездо USB (в COM плата не получает питание от кабеля C–C: нет
# резисторов CC), в загрузчик — кнопками BOOT+RST; обычный сброс после записи оставлял её в загрузчике
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 --after watchdog-reset write-flash 0x0 "$BIN"
