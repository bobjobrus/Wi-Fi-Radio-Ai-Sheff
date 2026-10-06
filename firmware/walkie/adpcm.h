// IMA ADPCM 4 бит: 320 отсчётов (20 мс при 16 кГц) → 163 байта.
// Кадр: предсказание int16 (LE) | индекс шага (1) | 160 байт полубайтов (первый отсчёт — младший).
// Каждый кадр несёт состояние кодера — потеря пакета не сбивает следующие.
// Эталон на Python: hub/wt_proto.py (AdpcmEncoder, adpcm_decode) — битовые потоки совпадают.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct AdpcmState {
  int16_t pred = 0;
  uint8_t index = 0;
};

// n — чётное; out: 3 + n/2 байт. Состояние st продолжается между кадрами.
void adpcm_encode_frame(AdpcmState& st, const int16_t* in, size_t n, uint8_t* out);
// in: 3 + n/2 байт → n отсчётов
void adpcm_decode_frame(const uint8_t* in, size_t n, int16_t* out);
