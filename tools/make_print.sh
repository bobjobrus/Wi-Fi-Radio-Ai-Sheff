#!/bin/bash
# Готовые к печати проекты Bambu Studio для P2S (сопло 0,4, Bambu PLA Matte, 0,16 Standard, наружная стенка 120 мм/с,
# 3 стенки, 15 %). С 28.09.2026: пластик у автора — PLA Matte (поток 1,01 против 0,98 у Basic), слой 0,16 глаже
# на скосах; наружная стенка медленнее ради поверхности — лицевая часть ≈2 ч (0,16 High Quality — 3 ч, разница мала).
# Белый диск динамика (stl/lens.stl) — отдельный стол, белым PLA Basic (30.09.2026: 3 шт. на три рации).
#   tools/make_print.sh   → print/*.3mf (ONLY=4_ — только столы, в имени которых есть «4_») (нарезаны, с оценкой времени и граммов); отправлять из Bambu Studio → Print plate
# Грабли: командной строке Bambu Studio нельзя давать системные профили как есть — она не разворачивает
# «inherits» и шаблоны g-code («include»), и получается стол 200×200 и чужой стартовый код.
# Поэтому профили сначала разворачивает tools/bambu_profile.py.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P=$(mktemp -d)
python3 "$ROOT/tools/bambu_profile.py" machine "Bambu Lab P2S 0.4 nozzle" "$P/machine.json"
# 29.09.2026: комплект 1 печатали 0.12mm High Quality (7 ч); для раций 2–3 автор выбрал 0.16mm High Quality
PROFILE="${PROFILE:-0.16mm High Quality @BBL P2S}"
python3 "$ROOT/tools/bambu_profile.py" process "$PROFILE" "$P/process.json" \
  '{"wall_loops":"3","sparse_infill_density":"15%","curr_bed_type":"Textured PEI Plate","name":"'"$PROFILE"' — рация"}'
# то же + кайма 5 мм (brim) — для мелких деталей отдельным столом (29.09 планка микрофона не напечаталась в комплекте)
python3 "$ROOT/tools/bambu_profile.py" process "$PROFILE" "$P/process_brim.json" \
  '{"wall_loops":"3","sparse_infill_density":"15%","curr_bed_type":"Textured PEI Plate","brim_type":"outer_only","brim_width":"5","name":"'"$PROFILE"' — рация, кайма"}'
python3 "$ROOT/tools/bambu_profile.py" filament "Bambu PLA Matte @BBL P2S" "$P/filament.json"
# белый диск динамика — белым PLA Basic (через него светит кольцо)
python3 "$ROOT/tools/bambu_profile.py" filament "Bambu PLA Basic @BBL P2S" "$P/filament_basic.json"
BS=/Applications/BambuStudio.app/Contents/MacOS/BambuStudio
OUT="$ROOT/print"; mkdir -p "$OUT"
TMP=$(mktemp -d)
S="$ROOT/stl"      # STL выгружены из cad/radio_handheld.scad (части *_print — уже в позе печати)
slice() {   # имя файла, затем детали
  local name="$1"; shift
  [ -n "${ONLY:-}" ] && [[ "$name" != *"$ONLY"* ]] && return 0   # ONLY=часть_имени — нарезать только этот стол
  local d="$TMP/$name"; mkdir -p "$d"
  (cd "$d" && "$BS" --orient 0 --arrange 1 --load-settings "$P/machine.json;${PROC:-$P/process.json}" --load-filaments "${FIL:-$P/filament.json}" \
        --slice 0 --outputdir "$d" --export-3mf "$name.3mf" "$@" > "$d/log.txt" 2>&1)
  cp "$d/$name.3mf" "$OUT/$name.3mf"
  python3 - "$d/result.json" "$name" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); p = r['sliced_plates'][0]
g = sum(f['total_used_g'] for f in p['filaments']); t = p['main_predication']
print('%-28s %s  %5.0f г  %2d ч %02d мин  деталей %d' % (sys.argv[2], r['error_string'], g, t // 3600, t % 3600 // 60, len(p['objects'])))
PY
}
# v3 (28.09.2026): одна рация = один стол; печатать 3 раза. Белый диск (stl/lens.stl) — отдельно белым.
slice "0_пробник_кромки_вала" "$S/edge_test.stl" "$S/shaft_gauge.stl" "$S/knob.stl"
slice "1_рация_v3_комплект" "$S/front.stl" "$S/back.stl" "$S/shelf.stl" "$S/spk_puck.stl" "$S/dk_arm.stl" "$S/mic_cap.stl" "$S/spacer.stl" "$S/knob.stl"
# 29.09.2026: планка микрофона отдельно — 2 шт. (вторая про запас), лапки 1,7 вместо 1,2, кайма 5 мм
PROC="$P/process_brim.json" slice "2_планка_микрофона_x2" "$S/mic_cap.stl" "$S/mic_cap.stl"
# 30.09.2026: белые диски динамика на все три рации — белым PLA Basic, отдельный стол
FIL="$P/filament_basic.json" slice "3_белый_диск_x3" "$S/lens.stl" "$S/lens.stl" "$S/lens.stl"
# 01.10.2026: комплект рации целиком белым PLA Basic — вместе с диском динамика на одном столе
FIL="$P/filament_basic.json" slice "4_рация_белая_комплект_с_диском" "$S/front.stl" "$S/back.stl" "$S/shelf.stl" "$S/spk_puck.stl" "$S/dk_arm.stl" "$S/mic_cap.stl" "$S/spacer.stl" "$S/knob.stl" "$S/lens.stl"
