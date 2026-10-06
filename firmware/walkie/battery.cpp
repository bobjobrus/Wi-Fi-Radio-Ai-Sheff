#include "battery.h"
#include <Arduino.h>
#include "audio.h"
#include "battery_est.h"
#include "config.h"
#include "state.h"

static BatteryEstimator s_est(BAT_LOW_PCT);
static uint32_t s_last = 0, s_warn = 0;

void battery_begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);
}

// Один замер: первое чтение выбросить (конденсатор выборки АЦП подзаряжается через делитель 50 кОм),
// из 16 следующих отбросить по 4 крайних с каждой стороны и усреднить середину
static float read_mv() {
  analogReadMilliVolts(PIN_BAT);
  uint16_t v[16];
  for (int i = 0; i < 16; i++) v[i] = analogReadMilliVolts(PIN_BAT);
  for (int i = 1; i < 16; i++)
    for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
      uint16_t t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  uint32_t sum = 0;
  for (int i = 4; i < 12; i++) sum += v[i];
  return sum / 8.0f * BAT_DIVIDER;
}

void battery_loop() {
  uint32_t now = millis();
  if (s_last && now - s_last < 1000) return;
  s_last = now;
  // динамик, передача, обновление, поиск и подключение Wi-Fi — просадка.
  // WAIT_RELEASE (кнопку держат после обрыва) — не нагрузка
  Tx tx = g_st.tx;
  bool busy = tx == Tx::REQUEST || tx == Tx::CHIRP || tx == Tx::TALK || g_st.rx_active || g_st.updating ||
              g_st.link == Link::NO_WIFI;
  s_est.add(read_mv(), now, busy);
  g_st.bat_raw_mv = (uint16_t)s_est.last_raw();
  g_st.bat_mv = (uint16_t)s_est.mv();
  g_st.bat_pct = s_est.pct();
  g_st.bat_low = s_est.low();
  g_st.bat_measuring = s_est.measuring();
  // разряжается: сигнал раз в 5 минут, почти пусто — раз в минуту (не во время разговора)
  uint32_t every = g_st.bat_pct <= BAT_CRIT_PCT ? 60000 : 300000;
  if (g_st.bat_low && (!s_warn || now - s_warn > every) && g_st.tx == Tx::IDLE && !g_st.rx_active) {
    s_warn = now;
    audio_tone(Tone::LOW_BATTERY);
    logf("аккумулятор: %u%% (%u мВ)", g_st.bat_pct, g_st.bat_mv);
  }
}
