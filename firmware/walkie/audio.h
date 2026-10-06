// Звук: микрофон INMP441 (I2S0) → АРУ → ADPCM; приём → буфер → ADPCM → усилитель MAX98357A (I2S1).
#pragma once
#include <stdint.h>
#include "protocol.h"

enum class Tone : uint8_t { NONE, PERMIT, BUSY, ERROR, ROGER, CONNECTED, LOST, CLICK, LOW_BATTERY };

struct TxFrame {
  uint8_t level;
  uint8_t data[wt::AUDIO_ENC_LEN];
};

bool audio_begin();                       // false — I2S не запустился (см. журнал)
void audio_tone(Tone t);
void audio_set_streaming(bool on);        // сеть: нужны кадры с микрофона
bool audio_tx_pop(TxFrame& f);            // сеть: забрать готовый кадр
void audio_rx_frame(uint32_t talker, uint32_t burst, uint16_t seq, const uint8_t* frame, uint8_t level);
void audio_rx_end(uint32_t talker, uint32_t burst);
void audio_rx_stop();                     // сами начали говорить — чужой приём прервать
float audio_volume_gain();
void audio_test_tone(uint16_t hz);
void audio_howl_info(uint32_t& found, uint16_t& last_hz);   // свист между рациями: сколько раз находили, последняя частота
void audio_mic_raw(int32_t mn[2], int32_t mx[2], uint32_t& frames);   // диагностика: сырые отсчёты обоих каналов с прошлого вызова       // проверка передачи без микрофона: вместо него синус hz (0 — выкл)
